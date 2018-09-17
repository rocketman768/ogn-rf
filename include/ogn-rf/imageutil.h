#ifndef IMAGEUTIL_H
#define IMAGEUTIL_H

#include <ogn-rf/buffer.h>

#include <cmath>

template <class Float> // scale floating-point data to 8-bit gray scale image
 void LogImage(SampleBuffer<uint8_t> &Image, SampleBuffer<Float> &Data, Float LogRef=0, Float Scale=1, Float Bias=0)
{ Image.Allocate(Data);
  int Pixels=Data.Full;
  for(int Idx=0; Idx<Pixels; Idx++)
  { Float Pixel=Data.Data[Idx];
    if(LogRef)
    { if(Pixel) { Pixel=logf((Float)Pixel/LogRef); Pixel = Pixel*Scale + Bias; }
           else { Pixel=0; } } 
    else
    { Pixel = Pixel*Scale + Bias; }
    if(Pixel<0x00) Pixel=0x00;
    else if(Pixel>0xFF) Pixel=0xFF;
    Image.Data[Idx]=(uint8_t)Pixel;
  }
  Image.Full=Pixels;
}

#endif /*IMAGEUTIL_H*/

