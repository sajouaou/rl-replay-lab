#pragma once
// Drawing over the game (BakkesMod's canvas): the teams' colours, centered text, and text the game's font can write.
#include "bakkesmod/wrappers/includes.h"

#include <string>

namespace lab {

// the teams' colours on screen (scoreboard, name tags, banners): blue, orange
struct Rgb {
    int r, g, b;
};
constexpr Rgb TEAM_RGB[2] = {{25, 85, 215}, {235, 115, 20}};

inline void Color(CanvasWrapper& c, int r, int g, int b, int a) {
    c.SetColor(static_cast<char>(r), static_cast<char>(g), static_cast<char>(b), static_cast<char>(a));
}

inline void Color(CanvasWrapper& c, const Rgb& rgb, int a) { Color(c, rgb.r, rgb.g, rgb.b, a); }

// text centered across the screen
inline void Centered(CanvasWrapper& c, const std::string& text, float scale, float y) {
    const Vector2F size = c.GetStringSize(text, scale, scale);
    c.SetPosition(Vector2F{(c.GetSize().X - size.X) / 2.f, y});
    c.DrawString(text, scale, scale, true);
}

// The game's font has no accents: French text and players' names in plain letters (others left out)
inline std::string Ascii(const std::string& text) {
    static const char* LATIN = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";   // U+00C0..U+00FF
    std::string out;
    for (size_t i = 0; i < text.size(); i++) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            out += static_cast<char>(c);
            continue;
        }
        const unsigned char d = i + 1 < text.size() ? static_cast<unsigned char>(text[i + 1]) : 0;
        const unsigned char e = i + 2 < text.size() ? static_cast<unsigned char>(text[i + 2]) : 0;
        if (c == 0xC3 && d >= 0x80 && d <= 0xBF) out += LATIN[d - 0x80];
        else if (c == 0xC2 && (d == 0xAB || d == 0xBB)) out += '"';            // guillemets
        else if (c == 0xC5 && (d == 0x92 || d == 0x93)) out += d == 0x92 ? "OE" : "oe";
        else if (c == 0xE2 && d == 0x80 && (e == 0x98 || e == 0x99)) out += '\'';
        else if (c == 0xE2 && d == 0x80 && (e == 0x93 || e == 0x94)) out += '-';
        else if (c == 0xE2 && d == 0x80 && e == 0xA6) out += "...";
        while (i + 1 < text.size() && (static_cast<unsigned char>(text[i + 1]) & 0xC0) == 0x80) i++;   // rest of the character
    }
    return out;
}

}  // namespace lab
