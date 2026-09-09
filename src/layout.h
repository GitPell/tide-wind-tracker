// GENERATED FILE -- do not edit by hand.
// Regenerated from layout.json and
// palette.json by tools/gen_layout_header.py,
// run automatically by `pio run` (see extra_scripts in platformio.ini).
// Edit those files instead.
#pragma once

#include <cstdint>

namespace layout {
  namespace palette {
    constexpr uint32_t RGB[6] = { 0x000000, 0xFFFFFF, 0xBF0000, 0xFFF338, 0x0000BF, 0x007C00 };
  }
  namespace screen {
    constexpr int W = 400;
    constexpr int H = 600;
  }
  namespace header {
    constexpr int HEIGHT = 56;
    constexpr int STATION_X = 12;
    constexpr int STATION_Y = 8;
    constexpr int DATETIME_X = 13;
    constexpr int DATETIME_Y = 38;
  }
  namespace now_strip {
    constexpr int Y0_OFFSET = 56;
    constexpr int LABEL_X = 12;
    constexpr int LABEL_Y = 10;
    constexpr int VALUE_X = 12;
    constexpr int VALUE_Y = 34;
    constexpr int UNIT_GAP_X = 6;
    constexpr int UNIT_Y = 58;
    constexpr int TREND_ARROW_X = 150;
    constexpr int TREND_ARROW_Y = 40;
    constexpr int TREND_WORD_X = 180;
    constexpr int TREND_WORD_Y = 48;
    constexpr int NEXT_RIGHT_MARGIN = 12;
    constexpr int NEXT_LABEL_DY = 30;
    constexpr int NEXT_TIME_DY = 46;
    constexpr int NEXT_VALUE_DY = 80;
  }
  namespace tide_box {
    constexpr int X0 = 12;
    constexpr int Y0 = 172;
    constexpr int RIGHT_MARGIN = 12;
    constexpr int Y1 = 336;
    constexpr int EVENT_MARKER_RADIUS = 3;
    constexpr int NOW_LINE_WIDTH = 2;
  }
  namespace wind {
    constexpr int Y = 352;
    constexpr int LABEL_X = 12;
    constexpr int COMPASS_CX = 78;
    constexpr int COMPASS_DY = 74;
    constexpr int COMPASS_R = 46;
    constexpr int COMPASS_LABEL_RADIUS_OFFSET = 11;
    constexpr int ARROW_TIP_INSET = 8;
    constexpr int ARROW_TAIL_INSET = 18;
    constexpr int ARROW_UNDERLAY_WIDTH = 9;
    constexpr int ARROW_COLOR_WIDTH = 5;
    constexpr int ARROW_BARB_ANGLE_DEG = 140;
    constexpr int ARROW_BARB_LENGTH = 16;
    constexpr int GUST_X = 152;
    constexpr int GUST_DY = 80;
    constexpr int FROM_X = 152;
    constexpr int FROM_DY = 106;
    constexpr int CHIP_RIGHT_OFFSET = 26;
    constexpr int CHIP_DY = 26;
    constexpr int CHIP_W = 12;
    constexpr int CHIP_H = 48;
  }
  namespace forecast {
    constexpr int X0 = 12;
    constexpr int RIGHT_MARGIN = 12;
    constexpr int TOP = 502;
    constexpr int BOTTOM = 566;
    constexpr int BAR_INSET = 1;
    constexpr int BAR_MIN_HEIGHT = 1;
    constexpr int BAR_HEIGHT_MARGIN = 12;
    constexpr int LABEL_DY_ABOVE_TOP = 18;
    constexpr int HOUR_LABEL_DY = 8;
  }
  namespace footer {
    constexpr int Y = 587;
    constexpr int LEFT_X = 12;
    constexpr int RIGHT_MARGIN = 12;
  }
  namespace firmware_only {
    namespace header {
      constexpr int READOUT_RIGHT_OFFSET = 155;
      constexpr int READOUT_Y = 20;
    }
    namespace wind {
      constexpr int READING_X = 152;
      constexpr int READING_DY = 20;
      constexpr int KT_GAP = 8;
      constexpr int KT_DY = 40;
    }
  }
}  // namespace layout
