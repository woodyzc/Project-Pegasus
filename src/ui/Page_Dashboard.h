#pragma once

#include "../system/PageManager/PageBase.h"

// Names pages are registered under with PageManager (see main.cpp).
#define PAGE_NAME_DASHBOARD "Dashboard"
#define PAGE_NAME_SETTINGS "Settings"

// The riding screen: speed, trip, time, incline, heart rate, route.
// Subscribes to DataCenter on load and renders from the Core-1 LVGL timer.
class PageDashboard : public PageBase {
public:
    PageDashboard();
    virtual ~PageDashboard() {}

    virtual void onViewLoad() override;
    virtual void onViewUnload() override;

    // The inline map shares its camera with the full-screen route page, and
    // this page is cached -- onViewLoad runs once for the life of the boot, so
    // it cannot be where that is picked up. These two fire on every visit.
    virtual void onViewWillAppear() override;
    virtual void onViewDidDisappear() override;
};

// Zero the trip accumulator. Called from the settings page; safe only from
// Core 1 (LVGL context), which is where both pages run.
// One gesture, three things: the odometer, the averages and maxima, and the
// file on the card all restart here, because all three describe one ride.
//
// Returns false if the ride log could not be told -- the numbers on screen
// still reset, so the caller has to say that the file did not split rather
// than report a clean start.
bool Page_Dashboard_StartNewRide();

// Shows or hides the second data page. Exists for the simulator, which cannot
// swipe: the renderer drives the same function the gesture does, so the frame
// it produces is the frame a rider gets.
void Page_Dashboard_ShowSecondPageForTest(bool on);
