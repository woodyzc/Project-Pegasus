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
};

// Zero the trip accumulator. Called from the settings page; safe only from
// Core 1 (LVGL context), which is where both pages run.
void Page_Dashboard_ResetTrip();
