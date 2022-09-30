// onevpl_sample.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include "encode.h"
#include "decode.h"

using namespace std;

int main()
{
    //decode();
    encode();

    cout << "main() done. press any key to quit." << endl;
    cin.get();
    return 0;
}
