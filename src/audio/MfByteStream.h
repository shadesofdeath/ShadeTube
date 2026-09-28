#pragma once
// IMFByteStream over a ProgressiveBuffer (internal to st_audio).
//
// - Synchronous Read blocks on the buffer until the bytes are downloaded.
// - BeginRead/EndRead (used by the MPEG-4 and WebM media sources) run the blocking read as a work
//   item on Media Foundation's MFASYNC_CALLBACK_QUEUE_LONG_FUNCTION queue, which exists precisely
//   for work that may block; the source's own work queues are never stalled.
// - Also exposes IMFAttributes carrying MF_BYTESTREAM_CONTENT_TYPE (the mime type) so the source
//   resolver picks the right byte-stream handler.
// Reads fail (E_ABORT / MF_E_NET_READ) when the buffer is cancelled, fails, or interruptWaiters()
// is called; the decoder treats that reader as broken and reopens it if needed.
#include <memory>
#include <string>

#include <mfidl.h>

namespace st::audio {

class ProgressiveBuffer;

// Creates the byte stream (refcount 1). The buffer's length must already be known.
HRESULT createMfByteStream(std::shared_ptr<ProgressiveBuffer> buffer, const std::string& mimeType,
                           IMFByteStream** out);

} // namespace st::audio
