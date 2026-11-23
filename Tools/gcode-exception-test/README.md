# GCodeException host test

This test checks the error message of a `GCodeException` that is thrown with the text of a reply buffer. Three functions do that:
- `Heater::SetTemperature` in `src/Heating/Heater.cpp`, when `SwitchOn` fails (`M104`, `M109`, `G10`, `M568`, `M140`, `M141`,
  `M190`, `M191`, a tool change). A local heater prints the reason itself (`LocalHeater::SwitchOn`), a heater on an expansion
  board gets it over CAN (`RemoteHeater::SwitchOn`).
- `SwitchEndstop::PrimeAxis` in `src/Endstops/SwitchEndstop.cpp`, when a switch on an expansion board cannot be enabled for a
  homing move (`G1 H1`).
- `CanInterface::EnableRemoteStallEndstop` in `src/CAN/CanInterface.cpp`, when a stall endstop on an expansion board cannot be
  enabled for a homing move.

`GCodeException` keeps a pointer to its message, which "should always point to a constant string in flash memory"
(`GCodeException.h`), and `GetMessage` uses the message as a printf format. The first two functions used to pass the text of a
local `String<StringLength100>`, so the exception pointed into a stack frame that is gone once the exception has left the
function. `GCodes::HandleGcode` keeps the exception of a `G1` move in the machine state with `GCodeMachineState::SetError` and
reports it only when the movement has stopped and the state machine is back to normal, so by then other calls have used that
stack. Where the exception is formatted straight away, the calls of `GetMessage` itself run on that stack. The third function used a
file-scope buffer instead, which the next call of the function clears or overwrites, also while an earlier error is still kept.
In all three, a `%` in the reply was taken as a format specifier.

The fix passes the text as the string parameter of a `"%s"` message, which the exception copies, as the parse errors that name
a variable already do. The test checks for each function that the reported message ends with exactly the text that the reply
buffer held when the function threw:
- after the exception has been kept as `GCodeMachineState::SetError` keeps it and the stack where the function ran has been used
  again (`UseStack`);
- for replies that the real code produces, quoted from RRF and Duet3Expansion, including ones longer than 50 characters: the CAN
  timeout message of `CanInterface::SendRequestAndGetCustomReply` and replies from an expansion board. The reply buffers hold
  100 characters, so the string parameter of `GCodeException` has to as well;
- for a reply with a `%` in it, which must come out as it is;
- for `EnableRemoteStallEndstop`, when the function runs again, successfully or with another error, before the first error is
  reported, as another homing move of the other motion system would.

The real `src/GCodes/GCodeException.cpp` and the string code of RRFLibraries (`StringRef`, `SafeVsnprintf`) are compiled
unchanged. The three functions need the heater, endstop and CAN classes, so the Makefile copies their text out of the sources
at build time (`extract-throwers.awk`), as `Tools/kinematics-test` does with `Move::SetKinematics`, and
`gcode_exception_test.cpp` gives them stand-ins for the classes and for the calls that fail. `shim/` holds stand-ins for the
headers that `GCodeException.cpp` includes; the string classes, `Bitmap`, the length constants of `src/Config/Configuration.h`,
`GCodeResult` and `RemoteInputHandle` are the real ones.

The AddressSanitizer build marks the reply buffer of the function that threw as unusable while the message is formatted, if the
buffer was a local variable. The compiler calls `__asan_handle_no_return` before each throw, which cleans up the shadow memory
of the stack, so without that ASan sees the read of the unwound frame only if its fake stack has reclaimed the frame. ASan
stops at the first bad read.

The Makefile reads the sources from this repository, `../..` (override with `RRF=`), and RRFLibraries and CANlib from next to it
(`RRFLIBS=`, `CANLIB=`). The firmware builds do not use this folder: make and CMake only read `src/`, and a resource filter in
`.project` hides it from Eclipse.

## Compile check

Every constructor of `GCodeException`, and every function that passes a message on to one, takes a `GCodeExceptionMessage`
(`GCodeException.h`). Its constructor is `consteval`, so a message that is not a compile-time constant does not compile, and the
firmware build fails at the next site that passes a run-time pointer, e.g. the text of a local or `String` buffer (the address of
a static char array is a constant and is not caught). `compile-check/run.sh` compiles the real
`GCodeException.h` with the stand-ins in `shim/`:
- `compile-check/accept.cpp` must compile: literals, a `constexpr` named message, a function that returns a constant or null
  message, and text that is built at run time passed as the string parameter of a `"%s"` message;
- each case of `compile-check/reject.cpp` must fail, with every error on the line of that case: a pointer known only at run
  time, the text of a reply buffer as `SwitchEndstop::PrimeAxis` and `Heater::SetTemperature` threw it, a `const` pointer that
  is not `constexpr`, a choice between two literals at run time, and a function that returns a plain pointer.

The other forwarders (`GCodeBuffer::ThrowGCodeException`, `ExpressionParser::ThrowParseException`, `ConstructParseException`,
`GCodeMachineState::SetError` and `SetWarning`) need classes that this test only stands in for, so only the firmware build checks
them.

## Build and run (WSL, g++ 13)

    make check        # build/gcode-exception-test and build/asan-gcode-exception-test (AddressSanitizer + UBSan), then run both,
                      # and run the compile check
    make compile-check

Each case prints `PASS` or `FAIL` with the reason. The exit code is 1 if any case fails.
