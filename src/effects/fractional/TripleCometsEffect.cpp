#include "TripleCometsEffect.h"

TripleCometsEffect::TripleCometsEffect()
{

}

void TripleCometsEffect::tick()
{
    //myMatrix->dimAll(220);
    myMatrix->dimAll(255 - settings.scale * 2);

    byte xx = 2 + sin8(millis() / 10) / 22;
    byte yy = 2 + cos8(millis() / 9) / 22;
    myMatrix->drawPixelXY(xx, yy, CRGB(0x0000FF));

    xx = 4 + sin8(millis() / 10) / 32;
    yy = 4 + cos8(millis() / 7) / 32;
    myMatrix->drawPixelXY(xx, yy, CRGB(0xFF0000));
    myMatrix->drawPixelXY(myMatrix->GetCenterX(), myMatrix->GetCenterY(), CRGB(0xFFFF00));

    e_x[0] += 3000;
    e_y[0] += 3000;
    e_z[0] += 3000;
    e_scaleX[0] = 8000;
    e_scaleY[0] = 8000;
    FillNoise(0);
    MoveFractionalNoiseX(2);
    MoveFractionalNoiseY(2, 0.33f);
}