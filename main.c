#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

#include <assert.h>
#include <math.h>

#include <time.h>
#include <signal.h>
#include <unistd.h>

#include <epoxy/gl.h>
#include <SDL.h>

#define W_SCREEN 800
#define H_SCREEN 600

// currently nonfunctional
#ifdef USE_SIMD
#include <emmintrin.h>
#include <tmmintrin.h>
#endif

typedef struct XSP_ {
	uint32_t debug_vram[64*32*2];
	int32_t xcomms;
	int32_t ram[2048*256];
	int32_t vram[1024*256];
	int32_t rom[512*256];
	int32_t sram[512*512];
	int32_t scratch[1*256];
	int32_t gpu_fifo[16];
	int32_t regs[32];
	int32_t c0_regs[32];
	int32_t rlo, rhi;
	int32_t lsaddr, lsreg, lsop;
	int32_t pc, pcdiff;
	int32_t fault_cause, fault_pc;
	int32_t seg7;
	int32_t i_stat, i_mask;
	int32_t gpu_stat, gpu_fifo_beg, gpu_fifo_end;
	int32_t gpu_y;
	int32_t dx_madr[8];
	int32_t dx_bcr[8];
	int32_t dx_chcr[8];
	int32_t dx_len[8];
	int32_t dx_xadr[8];
	int32_t dpcr, dicr, dma_enabled;
	int32_t spu_wptr;
} XSP;

uint8_t rom[512*1024];

SDL_Window *window = NULL;
SDL_GLContext window_gl;

GLuint xsp_prog;
GLuint xsp_comp_shader;

GLuint ssbo_xsp;

char *xsp_comp_src = NULL;
char *blit_vert_src = NULL;
char *blit_frag_src = NULL;

char *file_load_string(const char *fname)
{
	FILE *fp = fopen(fname, "rb");
	SDL_assert_release(fp != NULL);
	size_t buf_step = 1024*8;
	size_t buf_sz = buf_step;
	size_t accum = 0;
	char *buf = NULL;
	
	for(;;) {
		buf = realloc(buf, buf_sz+1);
		size_t amt = fread(buf+accum, 1, buf_sz-accum, fp);
		if(amt == 0) {
			buf_sz = accum;
			buf = realloc(buf, buf_sz+1);
			buf[buf_sz] = '\x00';
			return buf;
		}
		accum += amt;
		buf_sz = accum + buf_step;
	}
}

GLuint compile_shader(GLenum typ, const char *src)
{
	GLuint shader = glCreateShader(typ);
	const char *sa[1] = {src};
	glShaderSource(shader, 1, sa, NULL);
	glCompileShader(shader);
	char ilog[4096];
	glGetShaderInfoLog(shader, sizeof(ilog)-1, NULL, ilog);
	ilog[4095] = '\x00';
	printf("SHAD: {\n%s\n}\n\n", ilog);
	return shader;
}

int main(int argc, char *argv[])
{
	int x, y, i;

	// Set up SDL
	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_NOPARACHUTE);
	window = SDL_CreateWindow("XSP"
		, SDL_WINDOWPOS_CENTERED
		, SDL_WINDOWPOS_CENTERED
		, W_SCREEN
		, H_SCREEN
		, SDL_WINDOW_OPENGL);
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
	window_gl = SDL_GL_CreateContext(window);
	SDL_GL_SetSwapInterval(0);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	SDL_GL_SwapWindow(window);

	signal(SIGTERM, SIG_DFL);
	signal(SIGINT,  SIG_DFL);

	// Load image
	uint8_t font_src[H_SCREEN*W_SCREEN];
	{
		FILE *fp = fopen("cp437-gs.tga", "rb");
		fseek(fp, 18, SEEK_CUR);
		fread(font_src, 1, 256*8*8, fp);
		fclose(fp);
	}

	// Load ROM
	{
		FILE *fp = fopen("scph5502.bin", "rb");
		fread(rom, 1, sizeof(rom), fp);
		fclose(fp);
	}

	// Set up VBO
	GLfloat vadata[] = {
		0.0f, 1.0f, -1.0f, -1.0f,
		1.0f, 1.0f,  1.0f, -1.0f,
		0.0f, 0.0f, -1.0f,  1.0f,
		1.0f, 0.0f,  1.0f,  1.0f,
		0.0f, 0.0f, -1.0f,  1.0f,
		1.0f, 1.0f,  1.0f, -1.0f,
	};
	GLuint vbo = 0;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vadata), vadata, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	// Set up VAO
	GLuint vao = 0;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat)*4, &(((GLfloat *)0)[2]));
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat)*4, &(((GLfloat *)0)[0]));
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glBindVertexArray(0);

	// Set up shaders
	char ilog[4096];
	GLuint blit_prog = glCreateProgram();
	xsp_prog = glCreateProgram();

	blit_vert_src = file_load_string("blit.vert");
	blit_frag_src = file_load_string("blit.frag");
	GLuint shv = compile_shader(GL_VERTEX_SHADER, blit_vert_src);
	GLuint shf = compile_shader(GL_FRAGMENT_SHADER, blit_frag_src);
	glAttachShader(blit_prog, shv);
	glAttachShader(blit_prog, shf);
	glBindAttribLocation(blit_prog, 0, "v_vert");
	glBindAttribLocation(blit_prog, 1, "v_tc0");
	glBindFragDataLocation(blit_prog, 0, "o_col");
	glLinkProgram(blit_prog);
	glGetProgramInfoLog(blit_prog, sizeof(ilog)-1, NULL, ilog);
	ilog[4095] = '\x00';
	printf("PROG: {\n%s\n}\n\n", ilog);

	xsp_comp_src = file_load_string("xsp.comp");
	xsp_comp_shader = compile_shader(GL_COMPUTE_SHADER, xsp_comp_src);
	glAttachShader(xsp_prog, xsp_comp_shader);
	//glBindAttribLocation(prog, 0, "v_vert");
	//glBindAttribLocation(prog, 1, "v_tc0");
	//glBindFragDataLocation(prog, 0, "o_col");
	glLinkProgram(xsp_prog);
	glGetProgramInfoLog(xsp_prog, sizeof(ilog)-1, NULL, ilog);
	ilog[4095] = '\x00';
	printf("COMP: {\n%s\n}\n\n", ilog);

	// Set up textures
	GLuint tex_screen = 0;
	GLuint tex_font = 0;
	glGenTextures(1, &tex_screen);
	glGenTextures(1, &tex_font);
	glBindTexture(GL_TEXTURE_2D, tex_screen);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8UI, W_SCREEN, H_SCREEN);
	glBindTexture(GL_TEXTURE_2D, tex_font);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8UI, 16*8, 16*8);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16*8, 16*8, GL_RED_INTEGER, GL_UNSIGNED_BYTE, font_src);
	glBindTexture(GL_TEXTURE_2D, 0);

	// Set up SSBO
	int32_t xcomms = 1;
	glGenBuffers(1, &ssbo_xsp);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_xsp);
	glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(XSP), NULL, GL_DYNAMIC_COPY);
	glBufferSubData(GL_SHADER_STORAGE_BUFFER,
		((uint8_t *)&(((XSP *)0)->rom[0]))-(uint8_t *)0,
		sizeof(rom), rom);
	glBufferSubData(GL_SHADER_STORAGE_BUFFER,
		((uint8_t *)&(((XSP *)0)->xcomms))-(uint8_t *)0,
		sizeof(int32_t), &xcomms);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, ssbo_xsp);

	Uint32 last_sample = SDL_GetTicks();
	int fps = 0;

	for(;;)
	{
		glUseProgram(xsp_prog);
		glBindImageTexture(0, tex_screen, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8UI);
		glBindImageTexture(1, tex_font, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
		glDispatchCompute(1, 1, 1);
		glUseProgram(0);
		glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
		//glMemoryBarrier(GL_ALL_BARRIER_BITS);

		glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glUseProgram(blit_prog);
		glUniform1i(glGetUniformLocation(blit_prog, "tex0"), 0);
		glActiveTexture(GL_TEXTURE0 + 1);
		glBindTexture(GL_TEXTURE_2D, tex_font);
		glActiveTexture(GL_TEXTURE0 + 0);
		glBindTexture(GL_TEXTURE_2D, tex_screen);
		glBindVertexArray(vao);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		glBindVertexArray(0);
		glBindTexture(GL_TEXTURE_2D, 0);
		glUseProgram(0);

		SDL_GL_SwapWindow(window);

		//SDL_Delay(10);
		usleep(100);
		//usleep(100000);
		//usleep(10000);
		Uint32 now = SDL_GetTicks();
		fps++;
		if(now - last_sample >= 1000U) {
			int d = (int)(now - last_sample);
			while(d >= 1000) {
				last_sample += 1000;
				d -= 1000;
			}
			char buf[32];
			snprintf(buf, 31, "XSP | FPS: %d", fps);
			buf[31] = '\x00';
			SDL_SetWindowTitle(window, buf);
			fps = 0;
		}
		//printf("honk\n");
		SDL_Event ev;
		while(SDL_PollEvent(&ev))
		switch(ev.type)
		{
			case SDL_QUIT:
				return 0;

		}
	}

	return 0;
}

