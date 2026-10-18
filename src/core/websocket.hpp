#pragma once

namespace vmix {

/*
 * obs-websocket vendor "video-mixes" (requests via CallVendorRequest, events via VendorEvent):
 *
 *   GetMixList                                   → { mixes: [Mix] }
 *   GetMix          { mixName | mixId }          → Mix
 *   StartOutput     { mixName | mixId, output }  output: "record" | "replay" | "stream"
 *   StopOutput      { mixName | mixId, output }
 *   ToggleOutput    { mixName | mixId, output }  → { active }
 *   SaveReplay      { mixName | mixId }
 *   GetItemVisibility { sceneName, sceneItemId | sourceName }          → { mixes: [{ mixName, mixId, visible }] }
 *   SetItemVisibility { sceneName, sceneItemId | sourceName, mixName | mixId, visible }
 *
 *   Mix = { mixId, mixName, kind: "program" | "source", sourceName,
 *           outputs: { record | replay | stream: { enabled, active, paused, reconnecting, trigger, path, error } } }
 *
 *   Events: OutputStateChanged { mixId, mixName, output, active, paused }, ReplaySaved { mixId, mixName, path }
 *
 * Every response carries "ok"; failures add "error" with a message.
 * Registered after all modules are loaded (obs-websocket must exist); does nothing without obs-websocket.
 */
void register_websocket_vendor();
void unregister_websocket_vendor();

} // namespace vmix
