// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Flags: --expose-wasm

function EmptyTest() {
  "use asm";
  function caller() {
    empty();
    return 11;
  }
  function empty() {
  }
  return {caller: caller};
}

assertEquals(11, _WASMEXP_.instantiateModuleFromAsm(
      EmptyTest.toString()).caller());


function IntTest() {
  "use asm";
  function sum(a, b) {
    a = a|0;
    b = b|0;
    var c = (b + 1)|0
    var d = 3.0;
    var e = d | 0;  // double conversion
    return (a + c + 1)|0;
  }

  function caller() {
    return sum(77,22) | 0;
  }

  return {caller: caller};
}

assertEquals(101, _WASMEXP_.instantiateModuleFromAsm(
      IntTest.toString()).caller());


function Float64Test() {
  "use asm";
  function sum(a, b) {
    a = +a;
    b = +b;
    return +(a + b);
  }

  function caller() {
    var a = +sum(70.1,10.2);
    var ret = 0|0;
    if (a == 80.3) {
      ret = 1|0;
    } else {
      ret = 0|0;
    }
    return ret|0;
  }

  return {caller: caller};
}

assertEquals(1, _WASMEXP_.instantiateModuleFromAsm(
      Float64Test.toString()).caller());


function BadModule() {
  "use asm";
  function caller(a, b) {
    a = a|0;
    b = b+0;
    var c = (b + 1)|0
    return (a + c + 1)|0;
  }

  function caller() {
    return call(1, 2)|0;
  }

  return {caller: caller};
}

assertThrows(function() {
  _WASMEXP_.instantiateModuleFromAsm(BadModule.toString()).caller();
});


function TestReturnInBlock() {
  "use asm";

  function caller() {
    if(1) {
      {
        {
          return 1;
        }
      }
    }
    return 0;
  }

  return {caller: caller};
}

assertEquals(1, _WASMEXP_.instantiateModuleFromAsm(
      TestReturnInBlock.toString()).caller());


function TestWhileSimple() {
  "use asm";

  function caller() {
    var x = 0;
    while(x < 5) {
      x = (x + 1)|0;
    }
    return x|0;
  }

  return {caller: caller};
}

assertEquals(5, _WASMEXP_.instantiateModuleFromAsm(
      TestWhileSimple.toString()).caller());


function TestWhileWithoutBraces() {
  "use asm";

  function caller() {
    var x = 0;
    while(x <= 3)
      x = (x + 1)|0;
    return x|0;
  }

  return {caller: caller};
}

assertEquals(4, _WASMEXP_.instantiateModuleFromAsm(
      TestWhileWithoutBraces.toString()).caller());


function TestReturnInWhile() {
  "use asm";

  function caller() {
    var x = 0;
    while(x < 10) {
      x = (x + 6)|0;
      return x|0;
    }
    return x|0;
  }

  return {caller: caller};
}

assertEquals(6, _WASMEXP_.instantiateModuleFromAsm(
      TestReturnInWhile.toString()).caller());


function TestReturnInWhileWithoutBraces() {
  "use asm";

  function caller() {
    var x = 0;
    while(x < 5)
      return 7;
    return x|0;
  }

  return {caller: caller};
}

assertEquals(
    7, _WASMEXP_.instantiateModuleFromAsm(
      TestReturnInWhileWithoutBraces.toString()).caller());


function TestBreakInWhile() {
  "use asm";

  function caller() {
    while(1) {
      break;
    }
    return 8;
  }

  return {caller: caller};
}

assertEquals(8, _WASMEXP_.instantiateModuleFromAsm(
      TestBreakInWhile.toString()).caller());


function TestBreakInNestedWhile() {
  "use asm";

  function caller() {
    var x = 1.0;
    while(x < 1.5) {
      while(1)
        break;
      x = +(x + 0.25);
    }
    var ret = 0;
    if (x == 1.5) {
      ret = 9;
    }
    return ret|0;
  }

  return {caller: caller};
}

assertEquals(9, _WASMEXP_.instantiateModuleFromAsm(
      TestBreakInNestedWhile.toString()).caller());


function TestBreakInBlock() {
  "use asm";

  function caller() {
    var x = 0;
    abc: {
      x = 10;
      if (x == 10) {
        break abc;
      }
      x = 20;
    }
    return x|0;
  }

  return {caller: caller};
}

assertEquals(10, _WASMEXP_.instantiateModuleFromAsm(
      TestBreakInBlock.toString()).caller());


function TestBreakInNamedWhile() {
  "use asm";

  function caller() {
    var x = 0;
    outer: while (1) {
      x = (x + 1)|0;
      while (x == 11) {
        break outer;
      }
    }
    return x|0;
  }

  return {caller: caller};
}

assertEquals(11, _WASMEXP_.instantiateModuleFromAsm(
      TestBreakInNamedWhile.toString()).caller());


function TestContinue() {
  "use asm";

  function caller() {
    var x = 5;
    var ret = 0;
    while (x >= 0) {
      x = (x - 1)|0;
      if (x == 2) {
        continue;
      }
      ret = (ret - 1)|0;
    }
    return ret|0;
  }

  return {caller: caller};
}

assertEquals(-5, _WASMEXP_.instantiateModuleFromAsm(
      TestContinue.toString()).caller());


function TestContinueInNamedWhile() {
  "use asm";

  function caller() {
    var x = 5;
    var y = 0;
    var ret = 0;
    outer: while (x > 0) {
      x = (x - 1)|0;
      y = 0;
      while (y < 5) {
        if (x == 3) {
          continue outer;
        }
        ret = (ret + 1)|0;
        y = (y + 1)|0;
      }
    }
    return ret|0;
  }

  return {caller: caller};
}

assertEquals(20, _WASMEXP_.instantiateModuleFromAsm(
      TestContinueInNamedWhile.toString()).caller());


function TestNot() {
  "use asm";

  function caller() {
    var a = !(2 > 3);
    return a | 0;
  }

  return {caller:caller};
}

assertEquals(1, _WASMEXP_.instantiateModuleFromAsm(
      TestNot.toString()).caller());


function TestNotEquals() {
  "use asm";

  function caller() {
    var a = 3;
    if (a != 2) {
      return 21;
    }
    return 0;
  }

  return {caller:caller};
}

assertEquals(21, _WASMEXP_.instantiateModuleFromAsm(
      TestNotEquals.toString()).caller());


function TestUnsignedComparison() {
  "use asm";

  function caller() {
    var a = 0xffffffff;
    if ((a>>>0) > (0>>>0)) {
      return 22;
    }
    return 0;
  }

  return {caller:caller};
}

assertEquals(22, _WASMEXP_.instantiateModuleFromAsm(
      TestUnsignedComparison.toString()).caller());


function TestMixedAdd() {
  "use asm";

  function caller() {
    var a = 0x80000000;
    var b = 0x7fffffff;
    var c = 0;
    c = ((a>>>0) + b)|0;
    if ((c >>> 0) > (0>>>0)) {
      if (c < 0) {
        return 23;
      }
    }
    return 0;
  }

  return {caller:caller};
}

assertEquals(23, _WASMEXP_.instantiateModuleFromAsm(
      TestMixedAdd.toString()).caller());


function TestInt32HeapAccess(stdlib, foreign, buffer) {
  "use asm";

  var m = new stdlib.Int32Array(buffer);
  function caller() {
    var i = 4;

    m[0] = (i + 1) | 0;
    m[i >> 2] = ((m[0]|0) + 1) | 0;
    m[2] = ((m[i >> 2]|0) + 1) | 0;
    return m[2] | 0;
  }

  return {caller: caller};
}

assertEquals(7, _WASMEXP_.instantiateModuleFromAsm(
      TestInt32HeapAccess.toString()).caller());


function TestInt32HeapAccessExternal() {
  var memory = new ArrayBuffer(1024);
  var memory_int32 = new Int32Array(memory);
  var module = _WASMEXP_.instantiateModuleFromAsm(
      TestInt32HeapAccess.toString(), null, memory);
  module.__init__();
  assertEquals(7, module.caller());
  assertEquals(7, memory_int32[2]);
}

TestInt32HeapAccessExternal();


function TestHeapAccessIntTypes() {
  var types = [
    [Int8Array, 'Int8Array', '>> 0'],
    [Uint8Array, 'Uint8Array', '>> 0'],
    [Int16Array, 'Int16Array', '>> 1'],
    [Uint16Array, 'Uint16Array', '>> 1'],
    [Int32Array, 'Int32Array', '>> 2'],
    [Uint32Array, 'Uint32Array', '>> 2'],
  ];
  for (var i = 0; i < types.length; i++) {
    var code = TestInt32HeapAccess.toString();
    code = code.replace('Int32Array', types[i][1]);
    code = code.replace(/>> 2/g, types[i][2]);
    var memory = new ArrayBuffer(1024);
    var memory_view = new types[i][0](memory);
    var module = _WASMEXP_.instantiateModuleFromAsm(code, null, memory);
    module.__init__();
    assertEquals(7, module.caller());
    assertEquals(7, memory_view[2]);
    assertEquals(7, _WASMEXP_.instantiateModuleFromAsm(code).caller());
  }
}

TestHeapAccessIntTypes();


function TestFloatHeapAccess(stdlib, foreign, buffer) {
  "use asm";

  var f32 = new stdlib.Float32Array(buffer);
  var f64 = new stdlib.Float64Array(buffer);
  var fround = stdlib.Math.fround;
  function caller() {
    var i = 8;
    var j = 8;
    var v = 6.0;

    // TODO(bradnelson): Add float32 when asm-wasm supports it.
    f64[2] = v + 1.0;
    f64[i >> 3] = +f64[2] + 1.0;
    f64[j >> 3] = +f64[j >> 3] + 1.0;
    i = +f64[i >> 3] == 9.0;
    return i|0;
  }

  return {caller: caller};
}

assertEquals(1, _WASMEXP_.instantiateModuleFromAsm(
      TestFloatHeapAccess.toString()).caller());


function TestFloatHeapAccessExternal() {
  var memory = new ArrayBuffer(1024);
  var memory_float64 = new Float64Array(memory);
  var module = _WASMEXP_.instantiateModuleFromAsm(
      TestFloatHeapAccess.toString(), null, memory);
  module.__init__();
  assertEquals(1, module.caller());
  assertEquals(9.0, memory_float64[1]);
}

TestFloatHeapAccessExternal();


function TestConvertI32() {
  "use asm";

  function caller() {
    var a = 1.5;
    if ((~~(a + a)) == 3) {
      return 24;
    }
    return 0;
  }

  return {caller:caller};
}

assertEquals(24, _WASMEXP_.instantiateModuleFromAsm(
      TestConvertI32.toString()).caller());


function TestConvertF64FromInt() {
  "use asm";

  function caller() {
    var a = 1;
    if ((+(a + a)) > 1.5) {
      return 25;
    }
    return 0;
  }

  return {caller:caller};
}

assertEquals(25, _WASMEXP_.instantiateModuleFromAsm(
      TestConvertF64FromInt.toString()).caller());


function TestConvertF64FromUnsigned() {
  "use asm";

  function caller() {
    var a = 0xffffffff;
    if ((+(a>>>0)) > 0.0) {
      if((+a) < 0.0) {
        return 26;
      }
    }
    return 0;
  }

  return {caller:caller};
}

assertEquals(26, _WASMEXP_.instantiateModuleFromAsm(
      TestConvertF64FromUnsigned.toString()).caller());


function TestModInt() {
  "use asm";

  function caller() {
    var a = -83;
    var b = 28;
    return ((a|0)%(b|0))|0;
  }

  return {caller:caller};
}

assertEquals(-27, _WASMEXP_.instantiateModuleFromAsm(
      TestModInt.toString()).caller());


function TestModUnsignedInt() {
  "use asm";

  function caller() {
    var a = 0x80000000;  //2147483648
    var b = 10;
    return ((a>>>0)%(b>>>0))|0;
  }

  return {caller:caller};
}

assertEquals(8, _WASMEXP_.instantiateModuleFromAsm(
      TestModUnsignedInt.toString()).caller());


function TestModDouble() {
  "use asm";

  function caller() {
    var a = 5.25;
    var b = 2.5;
    if (a%b == 0.25) {
      return 28;
    }
    return 0;
  }

  return {caller:caller};
}

assertEquals(28, _WASMEXP_.instantiateModuleFromAsm(
      TestModDouble.toString()).caller());


/*
TODO: Fix parsing of negative doubles
      Fix code to use trunc instead of casts
function TestModDoubleNegative() {
  "use asm";

  function caller() {
    var a = -34359738368.25;
    var b = 2.5;
    if (a%b == -0.75) {
      return 28;
    }
    return 0;
  }

  return {caller:caller};
}

assertEquals(28, _WASMEXP_.instantiateModuleFromAsm(
      TestModDoubleNegative.toString()).caller());
*/


function TestNamedFunctions() {
  "use asm";

  var a = 0.0;
  var b = 0.0;

  function add() {
    return +(a + b);
  }

  function init() {
    a = 43.25;
    b = 34.25;
  }

  return {init:init,
          add:add};
}

var module = _WASMEXP_.instantiateModuleFromAsm(TestNamedFunctions.toString());
module.init();
assertEquals(77.5, module.add());


function TestGlobalsWithInit() {
  "use asm";

  var a = 43.25;
  var b = 34.25;

  function add() {
    return +(a + b);
  }

  return {add:add};
}

var module = _WASMEXP_.instantiateModuleFromAsm(TestGlobalsWithInit.toString());
module.__init__();
assertEquals(77.5, module.add());


function TestForLoop() {
  "use asm"

  function caller() {
    var ret = 0;
    var i = 0;
    for (i = 2; i <= 10; i = (i+1)|0) {
      ret = (ret + i) | 0;
    }
    return ret|0;
  }

  return {caller:caller};
}

assertEquals(54, _WASMEXP_.instantiateModuleFromAsm(
      TestForLoop.toString()).caller());


function TestForLoopWithoutInit() {
  "use asm"

  function caller() {
    var ret = 0;
    var i = 0;
    for (; i < 10; i = (i+1)|0) {
      ret = (ret + 10) | 0;
    }
    return ret|0;
  }

  return {caller:caller};
}

assertEquals(100, _WASMEXP_.instantiateModuleFromAsm(
      TestForLoopWithoutInit.toString()).caller());


function TestForLoopWithoutCondition() {
  "use asm"

  function caller() {
    var ret = 0;
    var i = 0;
    for (i=1;; i = (i+1)|0) {
      ret = (ret + i) | 0;
      if (i == 11) {
        break;
      }
    }
    return ret|0;
  }

  return {caller:caller};
}

assertEquals(66, _WASMEXP_.instantiateModuleFromAsm(
      TestForLoopWithoutCondition.toString()).caller());


function TestForLoopWithoutNext() {
  "use asm"

  function caller() {
    var i = 0;
    for (i=1; i < 41;) {
      i = (i + 1) | 0;
    }
    return i|0;
  }

  return {caller:caller};
}

assertEquals(41, _WASMEXP_.instantiateModuleFromAsm(
      TestForLoopWithoutNext.toString()).caller());


function TestForLoopWithoutBody() {
  "use asm"

  function caller() {
    var i = 0;
    for (i=1; i < 45 ; i = (i+1)|0) {
    }
    return i|0;
  }

  return {caller:caller};
}

assertEquals(45, _WASMEXP_.instantiateModuleFromAsm(
      TestForLoopWithoutBody.toString()).caller());


function TestDoWhile() {
  "use asm"

  function caller() {
    var i = 0;
    var ret = 21;
    do {
      ret = (ret + ret)|0;
      i = (i + 1)|0;
    } while (i < 2);
    return ret|0;
  }

  return {caller:caller};
}

assertEquals(84, _WASMEXP_.instantiateModuleFromAsm(
      TestDoWhile.toString()).caller());


function TestConditional() {
  "use asm"

  function caller() {
    var x = 1;
    return ((x > 0) ? 41 : 71)|0;
  }

  return {caller:caller};
}

assertEquals(41, _WASMEXP_.instantiateModuleFromAsm(
      TestConditional.toString()).caller());


function TestSwitch() {
  "use asm"

  function caller() {
    var ret = 0;
    var x = 7;
    switch (x) {
      case 1: return 0;
      case 7: {
        ret = 12;
        break;
      }
      default: return 0;
    }
    switch (x) {
      case 1: return 0;
      case 8: return 0;
      default: ret = (ret + 11)|0;
    }
    return ret|0;
  }

  return {caller:caller};
}

assertEquals(23, _WASMEXP_.instantiateModuleFromAsm(
      TestSwitch.toString()).caller());


function TestSwitchFallthrough() {
  "use asm"

  function caller() {
    var x = 17;
    var ret = 0;
    switch (x) {
      case 17:
      case 14: ret = 39;
      case 1: ret = (ret + 3)|0;
      case 4: break;
      default: ret = (ret + 1)|0;
    }
    return ret|0;
  }

  return {caller:caller};
}

assertEquals(42, _WASMEXP_.instantiateModuleFromAsm(
      TestSwitchFallthrough.toString()).caller());


function TestNestedSwitch() {
  "use asm"

  function caller() {
    var x = 3;
    var y = -13;
    switch (x) {
      case 1: return 0;
      case 3: {
        switch (y) {
          case 2: return 0;
          case -13: return 43;
          default: return 0;
        }
      }
      default: return 0;
    }
    return 0;
  }

  return {caller:caller};
}

assertEquals(43, _WASMEXP_.instantiateModuleFromAsm(
      TestNestedSwitch.toString()).caller());


function TestInitFunctionWithNoGlobals() {
  "use asm";
  function caller() {
    return 51;
  }
  return {caller};
}

var module = _WASMEXP_.instantiateModuleFromAsm(
    TestInitFunctionWithNoGlobals.toString());
module.__init__();
assertEquals(51, module.caller());


function TestExportNameDifferentFromFunctionName() {
  "use asm";
  function caller() {
    return 55;
  }
  return {alt_caller:caller};
}

var module = _WASMEXP_.instantiateModuleFromAsm(
    TestExportNameDifferentFromFunctionName.toString());
module.__init__();
assertEquals(55, module.alt_caller());


function TestFunctionTableSingleFunction() {
  "use asm";

  function dummy() {
    return 71;
  }

  function caller() {
    return function_table[0&0]() | 0;
  }

  var function_table = [dummy]

  return {caller:caller};
}

assertEquals(71, _WASMEXP_.instantiateModuleFromAsm(
      TestFunctionTableSingleFunction.toString()).caller());


function TestFunctionTableMultipleFunctions() {
  "use asm";

  function inc1(x) {
    x = x|0;
    return (x+1)|0;
  }

  function inc2(x) {
    x = x|0;
    return (x+2)|0;
  }

  function caller() {
    if (function_table[0&1](50) == 51) {
      if (function_table[1&1](60) == 62) {
        return 73;
      }
    }
    return 0;
  }

  var function_table = [inc1, inc2]

  return {caller:caller};
}

assertEquals(73, _WASMEXP_.instantiateModuleFromAsm(
      TestFunctionTableMultipleFunctions.toString()).caller());


function TestFunctionTable() {
  "use asm";

  function add(a, b) {
    a = a|0;
    b = b|0;
    return (a+b)|0;
  }

  function sub(a, b) {
    a = a|0;
    b = b|0;
    return (a-b)|0;
  }

  function inc(a) {
    a = a|0;
    return (a+1)|0;
  }

  function caller(table_id, fun_id, arg1, arg2) {
    table_id = table_id|0;
    fun_id = fun_id|0;
    arg1 = arg1|0;
    arg2 = arg2|0;
    if (table_id == 0) {
      return funBin[fun_id&3](arg1, arg2)|0;
    } else if (table_id == 1) {
      return fun[fun_id&0](arg1)|0;
    }
    return 0;
  }

  var funBin = [add, sub, sub, add];
  var fun = [inc];

  return {caller:caller};
}

var module = _WASMEXP_.instantiateModuleFromAsm(TestFunctionTable.toString());
module.__init__();
assertEquals(55, module.caller(0, 0, 33, 22));
assertEquals(11, module.caller(0, 1, 33, 22));
assertEquals(9, module.caller(0, 2, 54, 45));
assertEquals(99, module.caller(0, 3, 54, 45));
assertEquals(23, module.caller(0, 4, 12, 11));
assertEquals(31, module.caller(1, 0, 30, 11));
