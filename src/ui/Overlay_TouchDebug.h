#pragma once

// A bring-up readout for the touch controller, drawn on lv_layer_top() so it
// survives every page change.
//
// It exists because "touch does not work" has three causes that look
// identical from the outside, on a board whose only input is the thing under
// test and with no usable serial console (CLAUDE.md §8):
//
//   1. the controller never answered -- wrong bus, wrong address, held in
//      reset, or a V2 board carrying a CST3530 instead of a CST328;
//   2. it answers but never reports a contact -- reads fine, senses nothing;
//   3. it reports contacts whose coordinates land somewhere other than the
//      finger -- the panel works and the mapping is wrong.
//
// The readout separates all three in one glance, which is the difference
// between one bench round and several. Guessing between them is exactly what
// §8 says costs the most time.
//
// Compiled out entirely with -D PEGASUS_TOUCH_DEBUG=0. It is on by default
// only while this board is being brought up; turn it off once touch is
// trusted, because it covers the bottom of every page.
void TouchDebug_Show();
