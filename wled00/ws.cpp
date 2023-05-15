#include "wled.h"

/*
 * WebSockets server for bidirectional communication
 */
#ifdef WLED_ENABLE_WEBSOCKETS

static volatile uint16_t wsLiveClientId = 0;        // WLEDMM added "static"
static volatile unsigned long wsLastLiveTime = 0;   // WLEDMM
//uint8_t* wsFrameBuffer = nullptr;

#ifdef WLEDMM_FASTPATH
#define WS_LIVE_INTERVAL 80   // WLEDMM reduced update interval, to have more time for LEDs
#else
#define WS_LIVE_INTERVAL 40
#endif

void wsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len)
{
  if(type == WS_EVT_CONNECT){
    //client connected
    DEBUG_PRINTLN(F("WS client connected."));
    sendDataWs(client);
  } else if(type == WS_EVT_DISCONNECT){
    //client disconnected
    if (client->id() == wsLiveClientId) wsLiveClientId = 0;
    DEBUG_PRINTLN(F("WS client disconnected."));
  } else if(type == WS_EVT_DATA){
    // data packet
    AwsFrameInfo * info = (AwsFrameInfo*)arg;
    if(info->final && info->index == 0 && info->len == len){
      // the whole message is in a single frame and we got all of its data (max. 1450 bytes)
      if(info->opcode == WS_TEXT)
      {
        if (len > 0 && len < 10 && data[0] == 'p') {
          // application layer ping/pong heartbeat.
          // client-side socket layer ping packets are unresponded (investigate)
          client->text(F("pong"));
          return;
        }

        bool verboseResponse = false;
        if (!requestJSONBufferLock(11)) return;

        DeserializationError error = deserializeJson(doc, data, len);
        JsonObject root = doc.as<JsonObject>();
        if (error || root.isNull()) {
          releaseJSONBufferLock();
          return;
        }
        if (root["v"] && root.size() == 1) {
          //if the received value is just "{"v":true}", send only to this client
          verboseResponse = true;
        } else if (root.containsKey("lv")) {
          wsLiveClientId = root["lv"] ? client->id() : 0;
        } else {
          verboseResponse = deserializeState(root);
        }
        releaseJSONBufferLock(); // will clean fileDoc

        // force broadcast in 500ms after updating client
        if (verboseResponse) {
          sendDataWs(client);
          lastInterfaceUpdate = millis() - (INTERFACE_UPDATE_COOLDOWN -500);
        } else {
          // we have to send something back otherwise WS connection closes
          client->text(F("{\"success\":true}"));
          lastInterfaceUpdate = millis() - (INTERFACE_UPDATE_COOLDOWN -500);
        }
      }
    } else {
      //message is comprised of multiple frames or the frame is split into multiple packets
      //if(info->index == 0){
        //if (!wsFrameBuffer && len < 4096) wsFrameBuffer = new uint8_t[4096];
      //}

      //if (wsFrameBuffer && len < 4096 && info->index + info->)
      //{

      //}

      if((info->index + len) == info->len){
        if(info->final){
          if(info->message_opcode == WS_TEXT) {
            client->text(F("{\"error\":9}")); //we do not handle split packets right now
          }
        }
      }
      DEBUG_PRINTLN(F("WS multipart message."));
    }
  } else if(type == WS_EVT_ERROR){
    //error was received from the other end
    DEBUG_PRINTLN(F("WS error."));

  } else if(type == WS_EVT_PONG){
    //pong message was received (in response to a ping request maybe)
    DEBUG_PRINTLN(F("WS pong."));

  }
}

void sendDataWs(AsyncWebSocketClient * client)
{
  if (!ws.count()) return;
  AsyncWebSocketMessageBuffer * buffer;

  if (!requestJSONBufferLock(12)) return;

  JsonObject state = doc.createNestedObject("state");
  serializeState(state);
  JsonObject info  = doc.createNestedObject("info");
  serializeInfo(info);

  size_t len = measureJson(doc);
  DEBUG_PRINTF("JSON buffer size: %u for WS request (%u).\n", doc.memoryUsage(), len);

  size_t heap1 = ESP.getFreeHeap();
  DEBUG_PRINT(F("heap ")); DEBUG_PRINTLN(ESP.getFreeHeap());
  #ifdef ESP8266
  if (len>heap1) {
    DEBUG_PRINTLN(F("Out of memory (WS)!"));
    return;
  }
  #else
    // DEBUG_PRINTF("%s min free stack %d\n", pcTaskGetTaskName(NULL), uxTaskGetStackHighWaterMark(NULL)); //WLEDMM
  #endif
  if (len < 1) return; // WLEDMM do not allocate 0 size buffer
  
  // WLEDMM use exceptions to catch out-of-memory errors
  #if __cpp_exceptions
  try{
    buffer = ws.makeBuffer(len); // will not allocate correct memory sometimes on ESP8266
  } catch(...) {
    buffer = nullptr;
  }
  #else
  buffer = ws.makeBuffer(len); // will not allocate correct memory sometimes on ESP8266
  #endif
  #ifdef ESP8266
  size_t heap2 = ESP.getFreeHeap();
  DEBUG_PRINT(F("heap ")); DEBUG_PRINTLN(ESP.getFreeHeap());
  #else
  size_t heap2 = 0; // ESP32 variants do not have the same issue and will work without checking heap allocation
  #endif
  if (!buffer || heap1-heap2<len) {
    releaseJSONBufferLock();
    DEBUG_PRINTLN(F("WS buffer allocation failed."));
    ws.closeAll(1013); //code 1013 = temporary overload, try again later
    ws.cleanupClients(0); //disconnect all clients to release memory
    ws._cleanBuffers();
    return; //out of memory
  }

  buffer->lock();
  serializeJson(doc, (char *)buffer->get(), len);

  DEBUG_PRINT(F("Sending WS data "));
  if (client) {
    client->text(buffer);
    DEBUG_PRINTLN(F("to a single client."));
  } else {
    ws.textAll(buffer);
    DEBUG_PRINTLN(F("to multiple clients."));
  }
  buffer->unlock();
  ws._cleanBuffers();

  releaseJSONBufferLock();
}

static bool sendLiveLedsWs(uint32_t wsClient)  // WLEDMM added "static"
{
  AsyncWebSocketClient * wsc = ws.client(wsClient);
  if (!wsc || wsc->queueLength() > 0) return false; //only send if queue free

  size_t used = strip.getLengthTotal();
#ifdef ESP8266
  constexpr size_t MAX_LIVE_LEDS_WS = 256U;
#else
 #if !defined(WLEDMM_FASTPATH)
  constexpr size_t MAX_LIVE_LEDS_WS = 4096U;  //WLEDMM use 4096 as max matrix size
 #else
  constexpr size_t MAX_LIVE_LEDS_WS = 2048U;  //WLEDMM use 2048 as max matrix size - reduce "effect hickups" due to long transmissions
 #endif
#endif
  size_t n = ((used -1)/MAX_LIVE_LEDS_WS) +1; //only serve every n'th LED if count over MAX_LIVE_LEDS_WS
  size_t pos = (strip.isMatrix ? 4 : 2);
  size_t bufSize = pos + (used/n)*3;
  //WLEDMM: no skipLines

  if (bufSize < 1) return(false); // WLEDMM should not happen
  //AsyncWebSocketMessageBuffer * wsBuf = ws.makeBuffer(bufSize);
  // WLEDMM protect against exceptions due to low memory 
  AsyncWebSocketMessageBuffer * wsBuf = nullptr;
#if __cpp_exceptions
  try{
#endif
    wsBuf = ws.makeBuffer(bufSize);
#if __cpp_exceptions
  } catch(...) {
#else
  if (wsBuf == nullptr) {    // 8266 does not support exceptions
#endif
    wsBuf = nullptr;
    DEBUG_PRINTLN(F("WS buffer allocation failed, dropping connections."));
    ws.closeAll(1013); //code 1013 = temporary overload, try again later
    ws.cleanupClients(0); //disconnect all clients to release memory
    ws._cleanBuffers();
  }
  
  if (!wsBuf) return false; //out of memory
  uint8_t* buffer = wsBuf->get();
  if (!buffer) return false; //out of memory

  wsBuf->lock();  // protect buffer from being cleaned by another WS instance
  buffer[0] = 'L';
  buffer[1] = 1; //version
#ifndef WLED_DISABLE_2D
  if (strip.isMatrix) {
    buffer[1] = 2; //version
    buffer[2] = min(Segment::maxWidth, (uint16_t) 255); // WLEDMM prevent overflow
    buffer[3] = min(Segment::maxHeight, (uint16_t) 255);
    //WLEDMM: no skipLines
  }
#endif

  for (size_t i = 0; pos < bufSize -2; i += n)
  {
  //WLEDMM: no skipLines
    uint32_t c = strip.getPixelColor(i);
    buffer[pos++] = qadd8(W(c), R(c)); //R, add white channel to RGB channels as a simple RGBW -> RGB map
    buffer[pos++] = qadd8(W(c), G(c)); //G
    buffer[pos++] = qadd8(W(c), B(c)); //B
  }

  wsc->binary(wsBuf);
  wsBuf->unlock();     // un-protect buffer
  ws._cleanBuffers();  // cleans up if the message is not added to any clients.
  return true;
}

void handleWs()
{
  if (millis() - wsLastLiveTime > WS_LIVE_INTERVAL)
  {
    #ifdef ESP8266
    ws.cleanupClients(3);
    #else
    ws.cleanupClients();
    #endif
    bool success = true;
    if (wsLiveClientId) success = sendLiveLedsWs(wsLiveClientId);
    wsLastLiveTime = millis();
    if (!success) wsLastLiveTime -= 20; //try again in 20ms if failed due to non-empty WS queue
  }
}

#else
void handleWs() {}
void sendDataWs(AsyncWebSocketClient * client) {}
#endif