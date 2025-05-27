# CSSE2310
My assignments for UQ's CSSE2310 course (Computer Systems Principles and Programming) taken semester 2 2024. This course covered advanced programming in C as well as an introduction to operating system architecture (Unix-based) (system calls, processes, threads, networking etc).
## Rank
6th out of 271 students
## Assignment Descriptions
### A1
Intro to C assignment - basic password strength checker. ```uqentropy``` measures the entropy (measure of password strength) of a password given on command line. User can specify extra arguments such as case checking, digit appending etc.
### A2
Debugging assignment. Students were given a pure binary executable from which 10 unknown strings must be correctly entered based on information found using GDB to debug the program as it executes.
### A3
Multi-processing assignment - file compression/decompression tool. ```uqzip``` is a program capable of compressing and decompressing files given on command line using different compression/decompression algorithims (```zip```, ```gzip``` etc). Based on command line arguments, ```uqzip``` can perform this compression/decompression sequentially or in parallel using multiple processes.
### A4
Multi-threaded, multi-process networking assignment - Chess game. ```uqchessserver``` accepts connections from clients and allows them to play chess against other human players or against a computer chess engine (```stockfish```). ```uqchessclient``` is used to create clients which connect to ```uqchessserver```. ```uqchessserver``` creates a new thread to each connected client as well as handles the creation, interaction and killing of the ```stockfish``` process.
