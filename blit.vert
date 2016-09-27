#version 430

in vec2 v_vert;
in vec2 v_tc0;
out vec2 f_tc0;

void main()
{
	gl_Position = vec4(v_vert, 0.0, 1.0);
	f_tc0 = v_tc0;
}

