#!/bin/sh
cc -g -O1 -o run main.c `sdl2-config --cflags --libs` -lm -lepoxy && \
./run 2>/dev/null && \
true

