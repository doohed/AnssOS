#include <stdint.h>

void hello() {
    const char hello[] = "===============================Welcome to fuckingOS================================";
    
    
    char *vidptr = (char*)0xb8000;
    
    
    uint8_t color = 0x06;
    
    unsigned int i = 0;
    unsigned int j = 0;
    
    while (i < 80) {
        vidptr[j] = hello[i];   
        vidptr[j + 1] = color;  
        i++;
        j += 2;
    }
}


void desc() {
    const char hello[] = "This is my small project, a small 0S that I'm developing";
    
    
    char *vidptr = (char*)0xb8140;
    
    
    uint8_t color = 0x07;
    
    unsigned int i = 0;
    unsigned int j = 0;
    
    while (i < 60) {
        vidptr[j] = hello[i];   
        vidptr[j + 1] = color;  
        i++;
        j += 2;
    }
}

extern "C" void main() {
    hello();
    desc();    
    // Loop forever to avoid exiting
    while (1) {}
}