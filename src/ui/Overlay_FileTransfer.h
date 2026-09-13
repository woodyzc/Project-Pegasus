#pragma once

// The modal shown while the WiFi file server is running.
//
// A modal on LVGL's top layer rather than a PageManager page, for two reasons
// that are really one: the card must have a single reader while the server is
// up (FileServer.h explains why), and a page the rider can navigate away from
// would let the map or the route picker start reading it again. The overlay
// covers the screen and swallows input, so there is nowhere else to go.
//
// It also has to survive being the only thing on screen: the way out is a
// restart, so this modal is the last UI of the session.

// Starts the server and shows the panel. If the server refuses to start, shows
// the reason instead and offers only a way back.
void Overlay_FileTransfer_Show();
