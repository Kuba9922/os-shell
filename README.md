# os-shell 

A small shell written in C with support for external commands, pipelines, basic I/O redirection, and background execution.
Developed as an Operating Systems project.

## What it supports
- external commands via fork + execvp
- pipelines with `|`
- input/output redirection: `<`, `>`, `>>`
- multiple pipelines in one line using `;`
- background execution using `&`
- a short notification when a background pipeline terminates (printed before the next prompt)

## Built-ins
- exit
- lecho: prints arguments
- lcd [dir]: changes directory 
- lls: lists files in the current directory
- lkill [-SIGNAL] PID: sends a signal (default: SIGTERM)