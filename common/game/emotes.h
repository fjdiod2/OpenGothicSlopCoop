#pragma once
#include <array>
#include <string_view>

struct EmoteDefinition { std::string_view label, animation; };
inline constexpr std::array<EmoteDefinition,12> Emotes = {{
  {"Yes / nod", "T_YES"}, {"No", "T_NO"}, {"Shrug", "T_DONTKNOW"},
  {"Greet (left hand)", "T_GREETLEFT"}, {"Greet (right hand)", "T_GREETRIGHT"},
  {"Bow politely", "T_GREETNOV"}, {"Come here", "T_COMEOVERHERE"},
  {"Go away", "T_GETLOST2"}, {"I'll get you", "T_IGETYOU"},
  {"Scratch head", "R_SCRATCHHEAD"}, {"Dance", "T_DANCE_01"},
  {"Afraid", "S_FEAR_VICTIM1"}
}};
