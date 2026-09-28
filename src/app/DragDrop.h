#pragma once
// Drag and drop of songs.
//
// Inside the app: rows dragged out of a track table (TrackTable) carry their tracks. A ghost (count + first title)
// follows the pointer and the widget under it is asked whether it takes them (DropTarget): the sidebar's playlists
// and Liked Songs, and the queue panel. From Windows Explorer: audio files and folders dragged onto the main window
// (an OLE drop target) are read on a worker into tracks (app/DroppedFiles) and play at once, or join the queue where
// they are dropped on the queue panel.
//
// Everything here runs on the UI thread (OLE calls the drop target from the message loop).
#include "app/AppContext.h"
#include "app/Router.h"

#include <string>
#include <vector>

namespace st::app {

struct DragPayload {
    std::vector<catalog::Track> tracks;   // songs dragged inside the app
    std::vector<std::wstring> files;      // paths dragged from Explorer (files and folders, not read yet)
    bool fromExplorer() const { return !files.empty(); }
};

// Implemented by widgets that take drops. Positions are window DIPs.
class DropTarget {
public:
    virtual ~DropTarget() = default;
    // The pointer is over the target (called on every move, and a few times a second while it rests there): true
    // when the payload can be dropped here; the target then shows where.
    virtual bool dragOver(const DragPayload& payload, gfx::Point windowPos) = 0;
    // The pointer left, or the drag ended / was cancelled: remove the highlight.
    virtual void dragLeave() = 0;
    virtual void drop(const DragPayload& payload, gfx::Point windowPos) = 0;
};

namespace dragdrop {

inline constexpr float kThreshold = 6;   // DIPs the pointer travels from the press before a row drag starts

// An in-app drag, driven by the source widget, which holds the mouse capture: begin() once the pointer moved past
// kThreshold, move() on every move, finish() on release (drops when a target takes it), cancel() on Escape or when
// the source goes away.
void begin(std::vector<catalog::Track> tracks, gfx::Point windowPos);
void move(gfx::Point windowPos);
void finish(gfx::Point windowPos);
void cancel();
bool active();

// Drop actions the targets share.
// The songs a sidebar row (Liked Songs or a playlist route) takes from `tracks`; empty = it isn't a target for them.
std::vector<catalog::Track> tracksForRow(const Route& row, const std::vector<catalog::Track>& tracks);
void dropOnRow(const Route& row, const std::vector<catalog::Track>& tracks);   // likes / adds, then a toast
// Puts the payload in front of order index `orderIndex` of the queue (songs at once, files once they are read). An
// empty queue starts playing them.
void dropOnQueue(int orderIndex, const DragPayload& payload);

} // namespace dragdrop

} // namespace st::app
