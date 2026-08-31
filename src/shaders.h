#pragma once
#include <Arduino.h>

// A shader renders the half-open row range [y0, y1) so the two cores can
// split the frame between them. Everything else about it is up to you --
// add a function, add a row to the table at the bottom of shaders.cpp.
struct Shader {
  const char* name;
  void (*render)(uint16_t* fb, int y0, int y1, float t);
  int  defaultPal;       // palette applied when this effect is selected;
                         // cycle away from it freely at runtime
};

extern const Shader shaders[];
extern const int    shaderCount;
