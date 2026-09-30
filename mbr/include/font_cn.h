#ifndef _FONT_CN_H_
#define _FONT_CN_H_

#define FONT_CN_COUNT 78

// 16x16 CN glyphs (2 bytes/row)
extern const unsigned char font_cn[FONT_CN_COUNT][32];
// CN Unicode codepoints (same order as font_cn)
extern const unsigned int font_cn_codepoints[FONT_CN_COUNT];
// ASCII 0x20-0x7E glyphs (8x16)
extern const unsigned char font_ascii[95][16];

#endif
