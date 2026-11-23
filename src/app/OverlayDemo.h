#pragma once
// Dev: SHADETUBE_OVERLAY_DEMO drives the overlay family (menus, submenus, dialogs, toasts, the command palette) at
// set times after startup, for --screenshot checks of their looks and animations (DEVELOPMENT.md 5.4). Steps are
// `<action>@<ms>[=<arg>]`, separated by ';':
//   menu=x,y      the track menu (the playing song, else a sample one) at window DIPs x,y
//   sleep=x,y     the sleep timer menu
//   key=<combo>   posts that key to the main window (plain keys: Down, Right, Escape, S...)
//   hover=x,y     the pointer rests at window DIPs x,y (Window::simulatePointer)
//   dialog        a delete confirmation dialog
//   toast=<text>  a toast ("!" first = an error); toastaction=<text>: one with an action button
//   palette[=q]   the command palette
//   stats         logs the acrylic captures made so far and the bytes held
namespace st::app::overlaydemo {

void schedule();   // App, once the main window exists

} // namespace st::app::overlaydemo
