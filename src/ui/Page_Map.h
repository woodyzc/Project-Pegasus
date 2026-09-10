#pragma once

#include "../system/PageManager/PageBase.h"

#define PAGE_NAME_MAP "Map"

// Offline breadcrumb map (CLAUDE.md §5): draws the GPX trail loaded from the
// SD card, with the rider's position at the centre.
//
// Only reachable when navigation is set to GPX -- in TBT mode there is no
// trail to draw, and the dashboard's ROUTE panel carries the turn instead.
class PageMap : public PageBase {
public:
    PageMap() {}
    virtual ~PageMap() {}

    virtual void onViewLoad() override;
    virtual void onViewUnload() override;
};
