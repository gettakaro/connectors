# Generic connector failure responses

Takaro's Generic WebSocket connector currently resolves a matched request from
the frame's `payload` after validating only actions with a response DTO. Its
void actions, including `kickPlayer`, `banPlayer`, `unbanPlayer`, `sendMessage`,
and `shutdown`, have no response DTO validation. The connector does not inspect
the frame type or a top-level `error` before resolving those actions. The
[upstream resolver and action map at commit `207719e1`](https://github.com/gettakaro/takaro/blob/207719e1c0895ac7f0b7da80f6619c1e7bbd24bb/packages/app-connector/src/lib/websocket.ts#L438-L539)
show this behavior. A `response` frame containing only `error` can therefore
make a failed kick appear successful; that also happened in the v21 live kick
diagnostic, where the player remained online and native `/kick` was never called.

The ARK sidecar answers successful requests normally. When parsing or action
execution fails, it sends no frame for that request and records only its bounded
request ID, known action, and error category in its own log. Takaro's pending
request then fails at its configured timeout, which defaults to
[10 seconds at the same upstream commit](https://github.com/gettakaro/takaro/blob/207719e1c0895ac7f0b7da80f6619c1e7bbd24bb/packages/app-connector/src/config.ts#L89-L94).
This is a fail-closed connector workaround: the API reports a timeout, not the
specific native failure reason. Other requests on the WebSocket continue.
Takaro would need to reject an explicit error frame to provide a faster,
specific failure response without dropping the request.
