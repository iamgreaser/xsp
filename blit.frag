#version 430

layout(binding = 0) uniform usampler2D tex_screen;
layout(binding = 1) uniform usampler2D tex_font;
layout(std430, binding = 1) buffer psxStorage {
	uint debug_vram[64*32*2];
};
in vec2 f_tc0;
out vec4 o_col;

layout(binding = 2) uniform atomic_uint debug_ypos;

void main()
{
	int ypos = (int(atomicCounter(debug_ypos))-25)&31;
	vec2 fb = f_tc0*vec2(40,25);
	ivec2 fbsel = ivec2(floor(fb));
	vec2 fbtc = fb - vec2(fbsel);
	fbsel.y = (fbsel.y + ypos)&31;
	int c = int(debug_vram[fbsel.y*64+fbsel.x]);
	ivec2 cv = ivec2(c&15, (c>>4)&15);
	fbtc += vec2(cv);
	fbtc *= 1.0/16.0;
	o_col = vec4(texture(tex_font, fbtc).r)/255.0;
}

