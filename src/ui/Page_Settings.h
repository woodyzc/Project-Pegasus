#pragma once

#include "../system/PageManager/PageBase.h"

// System settings: backlight, unit system, trip reset, and read-only device
// diagnostics. Pushed from the dashboard's gear button; Pop()s back.
class PageSettings : public PageBase {
public:
    PageSettings() {}
    virtual ~PageSettings() {}

    virtual void onViewLoad() override;
    virtual void onViewUnload() override;
};
