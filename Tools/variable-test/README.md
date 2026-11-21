# Variable value host test

This test runs RRF's real `src/ObjectModel/Variable.cpp` with the real `src/Platform/Heap.cpp`, `src/Platform/StringHandle.cpp`
and `src/Platform/ArrayHandle.cpp`, compiled unchanged, on a PC. `ExpressionValue`, `ObjectModel`, the array table entries and
`IoPort` are stand-ins in `shim/ObjectModel/ObjectModel.h`. They store CString, object, object model array and port values as
bare pointers and count the references of heap strings and arrays like the real ones. The rest of the shim comes from
`../heap-test/shim`.

The test checks that a variable never keeps a pointer into the object model. `var`, `global`, `set`, macro parameters and the
variables that DSF sets all end in `Variable::Assign` or `Variable::AssignIndexed`, and a variable can outlive the item it was
taken from:
- Objects: `heat.heaters` is an array of heater objects (`OBJECT_MODEL_ARRAY_VALUE(self->heaters[...])` in `Heat.cpp`), and M950 H
  and M308 delete heaters. `Variable::Assign` rejected a single object but copied an array of objects element by element. Arrays
  that contain objects, also as inner elements of a 2D array or as elements of `{...}`, `take()` and similar results, are now
  rejected with the same error.
- Object model arrays: the inner arrays of a 2D array, e.g. `heat.bedHeaterMapping` or `move.kinematics.forwardMatrix`, point at
  the object that owns them, and M669 deletes the kinematics. They are now copied to the heap, so the variable keeps the values
  it was given.
- Strings of type CString: `state.messageBox.message` points into the message box, which M292 deletes, and
  `move.extruders[].filament` points into the filament, whose name M701 overwrites. A variable that kept the pointer always showed
  the current name, so `move.extruders[0].filament != var.fil_prev` could never become true. The variable now keeps a heap copy.
- Ports: `sensors.analog[0].port` points into the sensor, which M308 deletes. The variable now keeps the pin name as a string,
  the form in which every user of a port value already sees it.

The test also checks that a rejected `var` leaves no heap slot behind (the name of the variable, and the array that was partly
built when an element was rejected), that a rejected `set` keeps the old value, and that `set var.b = var.a` still shares the
array.

The Makefile reads the sources from this repository, `../..` (override with `RRF=`). The firmware builds do not use this folder:
make and CMake only read `src/`, and a resource filter in `.project` hides it from Eclipse.

## Build and run (WSL, g++ 13)

    make check        # build/variable-test and build/asan-variable-test (AddressSanitizer + UBSan), then run both

Each case prints `PASS` or `FAIL` with the reason. The exit code is 1 if any case fails.
