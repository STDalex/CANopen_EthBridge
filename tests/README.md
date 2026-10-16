# Host-side tests

`test_canopen_decoder.c` is a dependency-free test executable for the pure
CANopen predefined-connection-set decoder. Build it with any host C11 compiler,
adding `../Core/Inc` to the include path and linking
`../Core/Src/canopen_decoder.c`.

Example with Clang:

```text
clang -std=c11 -Wall -Wextra -I../Core/Inc test_canopen_decoder.c ../Core/Src/canopen_decoder.c -o test_canopen_decoder
./test_canopen_decoder
```
