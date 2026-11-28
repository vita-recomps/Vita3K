// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
namespace ss_delta::frame_pacing {
constexpr bool supported(int fps){return fps==24||fps==30||fps==40||fps==45||fps==60;}
constexpr uint64_t frame_period_us(int fps){return fps==24?41666:fps==30?33333:fps==40?25000:fps==45?22222:fps==60?16000:0;}
constexpr bool known_period(uint64_t us){return us==41666||us==33333||us==25000||us==22222||us==16000;}
constexpr uint32_t flip_interval(int fps){return fps<=30?2:1;}
}
