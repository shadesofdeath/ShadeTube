#pragma once
// "Şarkıyı tanı": ShadeTube listens to what this PC plays (another app's sound: a video in the browser, a game, a
// stream) or to the microphone (music in the room), recognizes the song (src/recognize) and offers to play it, like it,
// add it to a playlist or start its radio. A dialog of the overlay family (app/RecognizeUi.cpp): the source switch,
// the listening view (a live level wave in the accent), the result card, the not-found / silent / failure states and
// the history of recognized songs (app/Recognition, recognized.json). Entry points: the button next to the Search
// page's search field, the command palette, the `recognize-song` shortcut (unbound by default; it may also get a
// global key, which brings the window up first).
//
// Capture runs only while the dialog listens: it starts when the dialog opens (the source last picked; the microphone
// only once the user has picked it) and stops with a result, after 12 s, on a source switch or when the dialog
// closes. UI thread.
namespace st::app {

void initSongRecognition();   // the shortcut's handler, the history, the dev demo (SHADETUBE_RECOGNIZE_DEMO)
void openSongRecognition();   // posted: safe from a click or key handler

} // namespace st::app
