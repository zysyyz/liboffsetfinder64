//
//  main.cpp
//  offsetfinder64
//
//  Created by tihmstar on 10.01.18.
//  Copyright © 2018 tihmstar. All rights reserved.
//

#include <iostream>
#include "offsetfinder64.hpp"

using namespace std;
using namespace tihmstar;

int main(int argc, const char * argv[]) {
    
    offsetfinder64 fi(argv[1]);
    
    {
        auto dsa = fi.find_nosuid_off();
        for (auto asd :dsa) {
            cout << hex << (void*)asd._location << endl;
        }
    }
    {
        patchfinder64::patch asd = fi.find_proc_enforce();
        cout << hex << (void*)asd._location << endl;
    }
    {
        patchfinder64::patch asd = fi.find_amfi_patch_offsets();
        cout << hex << (void*)asd._location << endl;
    }
    {
        patchfinder64::patch asd = fi.find_i_can_has_debugger_patch_off();
        cout << hex << (void*)asd._location << endl;
    }
    {
        patchfinder64::patch asd = fi.find_sandbox_patch();
        cout << hex << (void*)asd._location << endl;
    }
    {
        patchfinder64::patch asd = fi.find_amfi_substrate_patch();
        cout << hex << (void*)asd._location << endl;
    }
    {
        patchfinder64::patch asd = fi.find_cs_enforcement_disable_amfi();
        cout << hex << (void*)asd._location << endl;
    }
    

    
    std::cout << "Done!\n";
    return 0;
}