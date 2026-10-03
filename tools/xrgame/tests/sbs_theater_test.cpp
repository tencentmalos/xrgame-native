#include "../../../app/src/main/cpp/winlator/SbsTheater.h"
#include <cassert>
#include <limits>
#include <iostream>

static bool close(float a, float b) { return std::abs(a-b) < 0.00001f; }
int main() {
    SbsTheater theater;
    auto flat = theater.eye(1920, 0);
    assert(flat.width == 1920 && flat.x == 0);
    assert(close(flat.projectX(-0.7f), -0.7f) && close(flat.projectY(0.3f), 0.3f));
    theater.set(true, 1.6f, 2.0f);
    auto left = theater.eye(1920, 0), right = theater.eye(1920, 1);
    assert(left.width == 960 && right.x == 960 && right.width == 960);
    assert(left.offsetX > 0 && right.offsetX < 0 && close(left.offsetX, -right.offsetX));
    // Game geometry and a cursor remain aligned; screen pixels stay square.
    assert(close(left.scaleX * left.width, left.scaleY * 1920));
    assert(close(left.projectX(0.6f)-left.projectX(0.2f), right.projectX(0.6f)-right.projectX(0.2f)));
    assert(left.projectX(-1) > -1 && left.projectX(1) < 1);
    // Distance shrinks both the screen and binocular disparity; size only changes the screen.
    theater.set(true, 1.6f, 4.0f);
    auto far = theater.eye(1920, 0);
    assert(close(far.scaleX*2, left.scaleX) && close(far.offsetX*2, left.offsetX));
    theater.set(true, 3.2f, 2.0f);
    auto big = theater.eye(1920, 0);
    assert(close(big.scaleX, left.scaleX*2) && close(big.offsetX, left.offsetX));
    auto oddLeft = theater.eye(1919, 0), oddRight = theater.eye(1919, 1);
    assert(oddLeft.width == oddRight.x && oddLeft.width+oddRight.width == 1919);
    assert(close(oddRight.scaleX*oddRight.width, oddRight.scaleY*1919));
    theater.set(true, std::numeric_limits<float>::quiet_NaN(), 0);
    assert(theater.widthMeters == 1.6f && theater.distanceMeters == 1);
    theater.set(true, -3, std::numeric_limits<float>::infinity());
    assert(theater.widthMeters == 0.8f && theater.distanceMeters == 2);
    theater.set(false, 1.6f, 2);
    assert(close(theater.eye(1920, 0).projectX(0.2f), 0.2f));
    std::cout << "SBS theater projection: PASS\n";
}
