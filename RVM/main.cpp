#include <iostream>
#include <vector>
#include <limits.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>

#include <unistd.h>
#include <fcntl.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/termios.h>
#include <sys/mman.h>

using namespace std;

uint16_t memory[UINT16_MAX]; // this is to have 65536 bytes of space

enum
{
    keyboard_status = 0xFE00, // keyboard status
    keyboard_data = 0xFE02  // keyboard data
};

// this is for checking the key (boilerplate code)
uint16_t check_key()
{
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    return select(1, &readfds, NULL, NULL, &timeout) != 0;
}

// this is for unix related commands (boilerplate code)
struct termios original_tio;

void disable_input_buffering()
{
    tcgetattr(STDIN_FILENO, &original_tio);
    struct termios new_tio = original_tio;
    new_tio.c_lflag &= ~ICANON & ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
}

void restore_input_buffering()
{
    tcsetattr(STDIN_FILENO, TCSANOW, &original_tio);
}

// this is for catching interrupts
void handle_interrupt(int signal)
{
    restore_input_buffering();
    printf("\n");
    exit(-2);
}

// this is for reading the memory from the right location
uint16_t mem_read(uint16_t address)
{
    if (address == keyboard_status)
    {
        if (check_key())
        {
            memory[keyboard_status] = (1 << 15);
            memory[keyboard_data] = getchar();
        }
        else
        {
            memory[keyboard_status] = 0;
        }
    }
    return memory[address];
}

void mem_write(uint16_t address, uint16_t val)
{
    memory[address] = val;
}

uint16_t change_endianness(uint16_t x)
{
    return (x << 8) | (x >> 8);
}

void read_image_file(FILE* file)
{
    // The first 16 bits of the program file specify the address in memory where the program should start. This is placed into "origin"
    uint16_t origin;
    // reading and convertin endian-ness
    fread(&origin, sizeof(origin), 1, file);
    origin = change_endianness(origin);
    
    // single fread to get all the data
    uint16_t max_read = UINT16_MAX - origin;
    uint16_t* p = memory + origin;
    size_t read = fread(p, sizeof(uint16_t), max_read, file);
    
    // for each, we are swapping to the endian-ness that we need.
    while (read-- > 0)
    {
        *p = change_endianness(*p);
        ++p;
    }
}

// we use this function to actually read (we use traditional C here)
int read_image_data(const char* image_path)
{
    FILE* file = fopen(image_path, "rb");
    if (!file) { return 0; };
    read_image_file(file);
    fclose(file);
    return 1;
}

bool running = true;

// this allows us to do (flag) & x7 to get the correct flag
enum
{
    FL_POS = 1 << 0, // positive
    FL_ZRO = 1 << 1, // zero
    FL_NEG = 1 << 2, // negative
};

// the registers
enum
{
    R_R0 = 0,
    R_R1,
    R_R2,
    R_R3,
    R_R4,
    R_R5,
    R_R6,
    R_R7,
    R_PC,
    R_COND,
    R_COUNT
};

uint16_t reg[R_COUNT];

void update_flags(uint16_t r)
{
    if (reg[r] == 0)
    {
        reg[R_COND] = FL_ZRO;
    }
    else if (reg[r] >> 15 & 0x1)
    {
        reg[R_COND] = FL_NEG;
    }
    else
    {
        reg[R_COND] = FL_POS;
    }
}

uint16_t sign_extend(uint16_t x, int bit_count)
{
    // only if the beginning is 1 do we extend with 0xFFFF in the beginning
    if ((x >> (bit_count - 1)) & 1) {
        x |= (0xFFFF << bit_count);
    }
    return x;
}

void op_add(uint16_t instr){
    // we only consider 3 bits
    uint16_t r0 = (instr >> 9) & 0x7;
    uint16_t r1 = (instr >> 6) & 0x7;
    // only consider one bit
    uint16_t immediate = (instr >> 5) & 0x1;
    if(immediate){
        // we extend beyond 5 bits (which is 1F);
        uint16_t imm_val = sign_extend(instr & 0x1F, 5);
        reg[r0] = reg[r1] + imm_val;
    } else {
        uint32_t other_register = (instr & 0x7);
        reg[r0] = reg[r1] + reg[other_register];
    }
    
    update_flags(r0);
}

void op_not(uint16_t instr){
    uint16_t r0 = (instr >> 9) & 0x7;
    uint16_t r1 = (instr >> 6) & 0x7;
    reg[r0] = ~reg[r1];
    update_flags(r0);
}

void op_branch(uint16_t instr){
    uint16_t offset = (instr & 0x1FF);
    uint16_t flag = (instr >> 9) & 0x7;
    if (flag & reg[R_COND])
    {
        reg[R_PC] += offset;
    }
}

void op_jump(uint16_t instr){
    uint16_t r1 = (instr >> 6) & 0x7;
    reg[R_PC] = reg[r1];
}

void op_jump_reg(uint16_t instr){
    uint16_t long_flag = (instr >> 11) & 1;
    reg[R_R7] = reg[R_PC];
    if (long_flag)
    {
        uint16_t long_pc_offset = sign_extend(instr & 0x7FF, 11);
        reg[R_PC] += long_pc_offset;  /* JSR */
    }
    else
    {
        uint16_t r1 = (instr >> 6) & 0x7;
        reg[R_PC] = reg[r1]; /* JSRR */
    }
}

void op_load(uint16_t instr){
    uint16_t r0 = (instr >> 9) & 0x7;
    uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
    reg[r0] = mem_read(reg[R_PC] + pc_offset);
    update_flags(r0);
}

void op_load_register(uint16_t instr){
    uint16_t r0 = (instr >> 9) & 0x7;
    uint16_t r1 = (instr >> 6) & 0x7;
    uint16_t offset = sign_extend(instr & 0x3F, 6);
    reg[r0] = mem_read(reg[r1] + offset);
    update_flags(r0);
}

void op_lea(uint16_t instr){
    uint16_t r0 = (instr >> 9) & 0x7;
    uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
    reg[r0] = reg[R_PC] + pc_offset;
    update_flags(r0);
}

void op_store(uint16_t instr){
    uint16_t r0 = (instr >> 9) & 0x7;
    uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
    mem_write(reg[R_PC] + pc_offset, reg[r0]);
}

void op_store_indirect(uint16_t instr){
    uint16_t r0 = (instr >> 9) & 0x7;
    uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
    mem_write(mem_read(reg[R_PC] + pc_offset), reg[r0]);
}

void op_store_register(uint16_t instr){
    uint16_t r0 = (instr >> 9) & 0x7;
    uint16_t r1 = (instr >> 6) & 0x7;
    uint16_t offset = sign_extend(instr & 0x3F, 6);
    mem_write(reg[r1] + offset, reg[r0]);
}

void op_ldi(uint16_t instr) {
    uint16_t r0 = (instr >> 9) & 0x7;
    uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
    reg[r0] = mem_read(mem_read(reg[R_PC] + pc_offset));
    update_flags(r0);
}

void op_and(uint16_t instr) {
    uint16_t immediate = (instr >> 5) & 0x1;
    uint16_t r0 = (instr >> 9) & 0x7;
    uint16_t r1 = (instr >> 6) & 0x7;
    if(immediate){
        uint16_t imm_val = sign_extend(instr & 0x1F, 5);
        reg[r0] = reg[r1] & imm_val;
    } else {
        uint16_t other_register = (instr & 0x7);
        reg[r0] = reg[r1] & reg[other_register];
    }
}

void op_mul(uint16_t instr){
    uint16_t immediate = (instr >> 5) & 0x1;
    uint16_t r0 = (instr >> 9) & 0x7;
    uint16_t r1 = (instr >> 6) & 0x7;
    if(immediate){
        uint16_t imm_val = sign_extend(instr & 0x1F, 5);
        reg[r0] = (reg[r1] * imm_val) & 0xFFFF;
    } else {
        uint16_t other_register = (instr & 0x7);
        reg[r0] = (reg[r1] * reg[other_register]) & 0xFFFF;
    }
}

void op_div(uint16_t instr){
    uint16_t immediate = (instr >> 5) & 0x1;
    uint16_t r0 = (instr >> 9) & 0x7;
    uint16_t r1 = (instr >> 6) & 0x7;
    if(immediate){
        uint16_t imm_val = sign_extend(instr & 0x1F, 5);
        if(!imm_val) return;
        reg[r0] = (reg[r1] / imm_val) & 0xFFFF;
    } else {
        uint16_t other_register = (instr & 0x7);
        if(!reg[other_register]) return;
        reg[r0] = (reg[r1] / reg[other_register]) & 0xFFFF;
    }
}

// these are traps (and are stored before 0x3000)
enum
{
    TRAP_GETC = 0x20,
    TRAP_OUT = 0x21,
    TRAP_PUTS = 0x22,
    TRAP_IN = 0x23,
    TRAP_PUTSP = 0x24,
    TRAP_HALT = 0x25
};

void trap_getc(){
    reg[R_R0] = (uint16_t)getchar();
}

void trap_out(){
    putc((char)reg[R_R0], stdout);
    fflush(stdout);
}

void trap_puts(){
    uint16_t* c = memory + reg[R_R0];
    while (*c)
    {
        putc((char)*c, stdout);
        ++c;
    }
    fflush(stdout);
}

void trap_in(){
    printf("Type in a character: ");
    char c = getchar();
    putc(c, stdout);
    reg[R_R0] = (uint16_t)c;
}

void trap_puts_p(){
    uint16_t* c = memory + reg[R_R0];
    while (*c) // getting at each step.
    {
        char first = (*c) & 0xFF;
        putc(first, stdout);
        char second = (*c) >> 8;
        if (second) putc(second, stdout);
        c++;
    }
    fflush(stdout);
}

void trap_halt(){
    puts("HALTED!");
    fflush(stdout);
    running = false;
}

void op_trap(uint16_t instr){
    switch (instr & 0xFF)
    {
        case TRAP_GETC:
            trap_getc();
            break;
        case TRAP_OUT:
            trap_out();
            break;
        case TRAP_PUTS:
            trap_puts();
            break;
        case TRAP_IN:
            trap_in();
            break;
        case TRAP_PUTSP:
            trap_puts_p();
            break;
        case TRAP_HALT:
            trap_halt();
            break;
    }
}

template <unsigned op>
void ins(uint16_t instr)
{
    switch (op) {
        case 0:
            op_branch(instr);
            break;
        case 1:
            op_add(instr);
            break;
        case 2:
            op_load(instr);
            break;
        case 3:
            op_store(instr);
            break;
        case 4:
            op_jump_reg(instr);
            break;
        case 5:
            op_and(instr);
            break;
        case 6:
            op_load_register(instr);
            break;
        case 7:
            op_store_register(instr);
            break;
        case 8:
            op_mul(instr);
            break;
        case 9:
            op_not(instr);
            break;
        case 10:
            op_ldi(instr);
            break;
        case 11:
            op_store_indirect(instr);
            break;
        case 12:
            op_jump(instr);
            break;
        case 13:
            op_div(instr);
            break;
        case 14:
            op_lea(instr);
            break;
        case 15:
            op_trap(instr);
            break;
    }
}

// CHANGE to MUL; DIV
static void (*op_table[16])(uint16_t) = {
    ins<0>, ins<1>, ins<2>, ins<3>,
    ins<4>, ins<5>, ins<6>, ins<7>,
    NULL, ins<9>, ins<10>, ins<11>,
    ins<12>, NULL, ins<14>, ins<15>
};

int main(int argc, const char* argv[]) {
    // This is how to use the code
    if (argc < 2)
    {
        printf("rvm [image-file] ...\n");
        exit(2);
    }

    for (int j = 1; j < argc; ++j)
    {
        if (!read_image_data(argv[j]))
        {
            printf("failed to load image: %s\n", argv[j]);
            exit(1);
        }
    }
    
    enum { PC_START = 0x3000 };
    reg[R_PC] = PC_START;
    
    running = true;
    while(running){
        uint16_t instr = mem_read(reg[R_PC]++);
        uint16_t op = instr >> 12; // getting the actual instruction
        op_table[op](instr);
    }
    return 0;
}
