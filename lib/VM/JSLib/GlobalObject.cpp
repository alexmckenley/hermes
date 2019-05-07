/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
//===----------------------------------------------------------------------===//
/// \file
/// Initialize the global object ES5.1 15.1
//===----------------------------------------------------------------------===//
#include "hermes/VM/JSArrayBuffer.h"
#include "hermes/VM/JSDataView.h"
#include "hermes/VM/JSLib.h"
#include "hermes/VM/JSTypedArray.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/PrimitiveBox.h"
#include "hermes/VM/StringView.h"
#include "hermes/dtoa/dtoa.h"

#include "JSLibInternal.h"

namespace hermes {
namespace vm {

/// ES5.1 15.1.2.4
static CallResult<HermesValue>
isNaN(void *, Runtime *runtime, NativeArgs args) {
  auto res = toNumber(runtime, args.getArgHandle(runtime, 0));
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return HermesValue::encodeBoolValue(std::isnan(res->getNumber()));
}

/// ES5.1 15.1.2.5
static CallResult<HermesValue>
isFinite(void *, Runtime *runtime, NativeArgs args) {
  auto res = toNumber(runtime, args.getArgHandle(runtime, 0));
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto value = res->getDouble();
  return HermesValue::encodeBoolValue(std::isfinite(value));
}

/// Needed to construct Function.prototype.
static CallResult<HermesValue> emptyFunction(void *, Runtime *, NativeArgs) {
  return HermesValue::encodeUndefinedValue();
}

/// Given a character \p c in radix \p radix, checks if it's valid.
static bool isValidRadixChar(char16_t c, int radix) {
  // c is 0..9.
  if (c >= '0' && c <= '9') {
    return (radix >= 10 || c < '0' + radix);
  }
  c = letterToLower(c);
  return (radix > 10 && c >= 'a' && c < 'a' + radix - 10);
}

/// ES5.1 15.1.2.2 parseInt(string, radix)
static CallResult<HermesValue>
parseInt(void *, Runtime *runtime, NativeArgs args) {
  // toString(arg0).
  auto strRes = toString(runtime, args.getArgHandle(runtime, 0));
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto str = toHandle(runtime, std::move(*strRes));

  int radix = 10;
  bool stripPrefix = true;
  // If radix (arg1) is present and not undefined, toInt32(arg1).
  if (args.getArgCount() > 1 && !args.getArg(1).isUndefined()) {
    auto intRes = toInt32(runtime, args.getArgHandle(runtime, 1));
    if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    radix = static_cast<int>(intRes->getNumber());
    if (radix == 0) {
      radix = 10;
    } else if (radix < 2 || radix > 36) {
      return HermesValue::encodeNaNValue();
    } else if (radix != 16) {
      stripPrefix = false;
    }
  }

  auto strView = StringPrimitive::createStringView(runtime, str);
  auto begin = strView.begin();
  auto end = strView.end();

  // Remove leading whitespaces.
  while (begin != end &&
         (isWhiteSpaceChar(*begin) || isLineTerminatorChar(*begin))) {
    ++begin;
  }

  // Process sign.
  int sign = 1;
  if (begin != end && (*begin == u'+' || *begin == u'-')) {
    if (*begin == u'-') {
      sign = -1;
    }
    ++begin;
  }

  // Strip 0x or 0X for 16-radix number.
  if (stripPrefix && begin != end) {
    if (*begin == u'0') {
      ++begin;
      if (begin != end && letterToLower(*begin) == u'x') {
        ++begin;
        radix = 16;
      } else {
        --begin;
      }
    }
  }

  // Find the longest prefix that's still a valid int.
  auto realEnd = begin;
  for (; realEnd != end && isValidRadixChar(*realEnd, radix); ++realEnd) {
  }
  if (realEnd == begin) {
    // Return NaN if string has no digits.
    return HermesValue::encodeNaNValue();
  }

  return HermesValue::encodeDoubleValue(
      sign * parseIntWithRadix(strView.slice(begin, realEnd), radix));
}

// Check if str1 is a prefix of str2.
static bool isPrefix(StringView str1, StringView str2) {
  if (str1.length() > str2.length()) {
    return false;
  }
  for (auto first1 = str1.begin(), last1 = str1.end(), first2 = str2.begin();
       first1 != last1;
       ++first1, ++first2) {
    if (*first1 != *first2) {
      return false;
    }
  }
  return true;
}

/// ES5.1 15.1.2.3 parseFloat(string)
static CallResult<HermesValue>
parseFloat(void *, Runtime *runtime, NativeArgs args) {
  // toString(arg0).
  auto res = toString(runtime, args.getArgHandle(runtime, 0));
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  auto strHandle = toHandle(runtime, std::move(*res));
  auto origStr = StringPrimitive::createStringView(runtime, strHandle);

  auto &idTable = runtime->getIdentifierTable();

  // Trim leading whitespaces.
  auto begin = origStr.begin();
  auto end = origStr.end();

  while (begin != end &&
         (isWhiteSpaceChar(*begin) || isLineTerminatorChar(*begin))) {
    ++begin;
  }
  StringView str16 = origStr.slice(begin, end);

  // Check for special values.
  // parseFloat allows for partial match, hence we have to check for
  // substring.
  if (LLVM_UNLIKELY(isPrefix(
          idTable.getStringView(
              runtime, Predefined::getSymbolID(Predefined::Infinity)),
          str16))) {
    return HermesValue::encodeDoubleValue(
        std::numeric_limits<double>::infinity());
  }
  if (LLVM_UNLIKELY(isPrefix(
          idTable.getStringView(
              runtime, Predefined::getSymbolID(Predefined::PositiveInfinity)),
          str16))) {
    return HermesValue::encodeDoubleValue(
        std::numeric_limits<double>::infinity());
  }
  if (LLVM_UNLIKELY(isPrefix(
          idTable.getStringView(
              runtime, Predefined::getSymbolID(Predefined::NegativeInfinity)),
          str16))) {
    return HermesValue::encodeDoubleValue(
        -std::numeric_limits<double>::infinity());
  }
  if (LLVM_UNLIKELY(isPrefix(
          idTable.getStringView(
              runtime, Predefined::getSymbolID(Predefined::NaN)),
          str16))) {
    return HermesValue::encodeNaNValue();
  }

  // Copy 16 bit chars into 8 bit chars as long as the character is
  // still a valid decimal number character.
  auto len = str16.length();
  llvm::SmallVector<char, 32> str8(len + 1);
  uint32_t i = 0;
  for (auto c : str16) {
    if ((c >= u'0' && c <= u'9') || c == '.' || letterToLower(c) == 'e' ||
        c == '+' || c == '-') {
      str8[i] = static_cast<char>(c);
    } else {
      break;
    }
    ++i;
  }
  if (i == 0) {
    // Empty string.
    return HermesValue::encodeNaNValue();
  }
  // Use g_strtod to figure out the longest prefix that's still valid.
  // g_strtod will try to convert the string to int for as long as it can,
  // and set endPtr to the last location where the prefix so far is still
  // a valid integer.
  len = i;
  str8[len] = '\0';
  char *endPtr;
  ::g_strtod(str8.data(), &endPtr);
  if (endPtr == str8.data()) {
    // Empty string.
    return HermesValue::encodeNaNValue();
  }
  // Now we know the prefix untill endPtr is a valid int.
  *endPtr = '\0';
  return HermesValue::encodeDoubleValue(::g_strtod(str8.data(), &endPtr));
}

/// Customized global function. gc() forces a GC collect.
static CallResult<HermesValue> gc(void *, Runtime *runtime, NativeArgs) {
  runtime->collect();
  return HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue>
throwTypeError(void *ctx, Runtime *runtime, NativeArgs) {
  char *message = (char *)ctx;
  assert(message != nullptr && "[[ThrowTypeError]] requires a message");
  return runtime->raiseTypeError(TwineChar16(message));
}

// NOTE: when declaring more global symbols, don't forget to update
// "Libhermes.h".
void initGlobalObject(Runtime *runtime) {
  GCScope gcScope{runtime, "initGlobalObject", 256};

  // Not enumerable, not writable, not configurable.
  DefinePropertyFlags constantDPF{};
  constantDPF.setEnumerable = 1;
  constantDPF.setWritable = 1;
  constantDPF.setConfigurable = 1;
  constantDPF.setValue = 1;
  constantDPF.enumerable = 0;
  constantDPF.writable = 0;
  constantDPF.configurable = 0;

  // Not enumerable, but writable and configurable.
  DefinePropertyFlags normalDPF{};
  normalDPF.setEnumerable = 1;
  normalDPF.setWritable = 1;
  normalDPF.setConfigurable = 1;
  normalDPF.setValue = 1;
  normalDPF.enumerable = 0;
  normalDPF.writable = 1;
  normalDPF.configurable = 1;

  /// Clear the configurable flag.
  DefinePropertyFlags clearConfigurableDPF{};
  clearConfigurableDPF.setConfigurable = 1;
  clearConfigurableDPF.configurable = 0;

  // Define a function on the global object with name \p name.
  // Allocates a NativeObject and puts it in the global object.
  auto defineGlobalFunc =
      [&](SymbolID name, NativeFunctionPtr functionPtr, unsigned paramCount) {
        gcScope.clearAllHandles();

        auto func = NativeFunction::createWithoutPrototype(
            runtime, nullptr, functionPtr, name, paramCount);
        runtime->ignoreAllocationFailure(JSObject::defineOwnProperty(
            runtime->getGlobal(), runtime, name, normalDPF, func));
        return func;
      };

  // 15.1.1.1 NaN.
  runtime->ignoreAllocationFailure(JSObject::defineOwnProperty(
      runtime->getGlobal(),
      runtime,
      Predefined::getSymbolID(Predefined::NaN),
      constantDPF,
      runtime->makeHandle(HermesValue::encodeNaNValue())));

  // 15.1.1.2 Infinity.
  runtime->ignoreAllocationFailure(JSObject::defineOwnProperty(
      runtime->getGlobal(),
      runtime,
      Predefined::getSymbolID(Predefined::Infinity),
      constantDPF,
      runtime->makeHandle(HermesValue::encodeDoubleValue(
          std::numeric_limits<double>::infinity()))));

  // 15.1.1.2 undefined.
  runtime->ignoreAllocationFailure(JSObject::defineOwnProperty(
      runtime->getGlobal(),
      runtime,
      Predefined::getSymbolID(Predefined::undefined),
      constantDPF,
      runtime->makeHandle(HermesValue::encodeUndefinedValue())));

  // "Forward declaration" of Object.prototype. Its properties will be populated
  // later.

  runtime->objectPrototype =
      JSObject::create(runtime, runtime->makeNullHandle<JSObject>())
          .getHermesValue();
  runtime->objectPrototypeRawPtr = vmcast<JSObject>(runtime->objectPrototype);

  // "Forward declaration" of Error.prototype. Its properties will be populated
  // later.
  runtime->ErrorPrototype = runtime->ignoreAllocationFailure(JSError::create(
      runtime, Handle<JSObject>::vmcast(&runtime->objectPrototype)));

// "Forward declaration" of the prototype for native error types. Their
// properties will be populated later.
#define NATIVE_ERROR_TYPE(name)                                        \
  runtime->name##Prototype =                                           \
      JSObject::create(                                                \
          runtime, Handle<JSObject>::vmcast(&runtime->ErrorPrototype)) \
          .getHermesValue();
#include "hermes/VM/NativeErrorTypes.def"

  // "Forward declaration" of Function.prototype. Its properties will be
  // populated later.
  Handle<NativeFunction> funcRes = NativeFunction::create(
      runtime,
      Handle<JSObject>::vmcast(&runtime->objectPrototype),
      nullptr,
      emptyFunction,
      SymbolID{},
      0,
      runtime->makeNullHandle<JSObject>());
  runtime->functionPrototype = funcRes.getHermesValue();
  runtime->functionPrototypeRawPtr = funcRes.get();
  runtime->ignoreAllocationFailure(JSObject::defineOwnProperty(
      Handle<JSObject>::vmcast(&runtime->functionPrototype),
      runtime,
      Predefined::getSymbolID(Predefined::length),
      clearConfigurableDPF,
      runtime->getUndefinedValue()));

  // [[ThrowTypeError]].
  auto throwTypeErrorFunction = NativeFunction::create(
      runtime,
      Handle<JSObject>::vmcast(&runtime->functionPrototype),
      const_cast<void *>((const void *)"Restricted in strict mode"),
      throwTypeError,
      Predefined::getSymbolID(Predefined::emptyString),
      0,
      runtime->makeNullHandle<JSObject>());
  runtime->ignoreAllocationFailure(JSObject::defineOwnProperty(
      throwTypeErrorFunction,
      runtime,
      Predefined::getSymbolID(Predefined::length),
      clearConfigurableDPF,
      runtime->getUndefinedValue()));
  runtime->throwTypeErrorAccessor =
      runtime->ignoreAllocationFailure(PropertyAccessor::create(
          runtime, throwTypeErrorFunction, throwTypeErrorFunction));

  // Define the 'parseInt' function.
  runtime->parseIntFunction =
      defineGlobalFunc(
          Predefined::getSymbolID(Predefined::parseInt), parseInt, 2)
          .getHermesValue();

  // Define the 'parseFloat' function.
  runtime->parseFloatFunction =
      defineGlobalFunc(
          Predefined::getSymbolID(Predefined::parseFloat), parseFloat, 1)
          .getHermesValue();

  // "Forward declaration" of String.prototype. Its properties will be
  // populated later.
  runtime->stringPrototype = runtime->ignoreAllocationFailure(JSString::create(
      runtime,
      runtime->getPredefinedStringHandle(Predefined::emptyString),
      Handle<JSObject>::vmcast(&runtime->objectPrototype)));

  // "Forward declaration" of Number.prototype. Its properties will be
  // populated later.
  runtime->numberPrototype = runtime->ignoreAllocationFailure(JSNumber::create(
      runtime, +0.0, Handle<JSObject>::vmcast(&runtime->objectPrototype)));

  // "Forward declaration" of Boolean.prototype. Its properties will be
  // populated later.
  runtime->booleanPrototype =
      runtime->ignoreAllocationFailure(JSBoolean::create(
          runtime, false, Handle<JSObject>::vmcast(&runtime->objectPrototype)));

  // "Forward declaration" of Symbol.prototype. Its properties will be
  // populated later.
  runtime->symbolPrototype = JSObject::create(runtime).getHermesValue();

  // "Forward declaration" of Date.prototype. Its properties will be
  // populated later.
  runtime->datePrototype =
      JSObject::create(
          runtime, Handle<JSObject>::vmcast(&runtime->objectPrototype))
          .getHermesValue();

  // "Forward declaration" of %IteratorPrototype%.
  runtime->iteratorPrototype = JSObject::create(runtime).getHermesValue();

  // "Forward declaration" of Array.prototype. Its properties will be
  // populated later.
  auto arrRes = runtime->ignoreAllocationFailure(JSArray::create(
      runtime,
      Handle<JSObject>::vmcast(&runtime->objectPrototype),
      JSArray::createClass(
          runtime, Handle<JSObject>::vmcast(&runtime->objectPrototype)),
      0,
      0));
  runtime->arrayPrototype = arrRes;
  runtime->arrayPrototypeRawPtr = vmcast<JSObject>(runtime->arrayPrototype);

  // Declare the array class.
  runtime->arrayClass =
      JSArray::createClass(
          runtime, Handle<JSObject>::vmcast(&runtime->arrayPrototype))
          .getHermesValue();
  runtime->arrayClassRawPtr = vmcast<HiddenClass>(runtime->arrayClass);

  // "Forward declaration" of ArrayBuffer.prototype. Its properties will be
  // populated later.
  runtime->arrayBufferPrototype =
      JSObject::create(
          runtime, Handle<JSObject>::vmcast(&runtime->objectPrototype))
          .getHermesValue();

  // "Forward declaration" of DataView.prototype. Its properties will be
  // populated later.
  runtime->dataViewPrototype =
      JSObject::create(
          runtime, Handle<JSObject>::vmcast(&runtime->objectPrototype))
          .getHermesValue();

  // "Forward declaration" of TypedArrayBase.prototype. Its properties will be
  // populated later.
  runtime->typedArrayBasePrototype =
      JSTypedArrayBase::create(
          runtime, Handle<JSObject>::vmcast(&runtime->objectPrototype))
          .getHermesValue();

// Typed arrays
// NOTE: a TypedArray's prototype is a normal object, not a TypedArray.
#define TYPED_ARRAY(name, type)                                        \
  runtime->name##ArrayPrototype =                                      \
      JSObject::create(                                                \
          runtime,                                                     \
          Handle<JSObject>::vmcast(&runtime->typedArrayBasePrototype)) \
          .getHermesValue();
#include "hermes/VM/TypedArrays.def"

  // "Forward declaration" of Set.prototype. Its properties will be
  // populated later.
  runtime->setPrototype = runtime->ignoreAllocationFailure(JSSet::create(
      runtime, Handle<JSObject>::vmcast(&runtime->objectPrototype)));

  runtime->setIteratorPrototype =
      createSetIteratorPrototype(runtime).getHermesValue();

  // "Forward declaration" of Map.prototype. Its properties will be
  // populated later.
  runtime->mapPrototype = runtime->ignoreAllocationFailure(JSMap::create(
      runtime, Handle<JSObject>::vmcast(&runtime->objectPrototype)));

  runtime->mapIteratorPrototype =
      createMapIteratorPrototype(runtime).getHermesValue();

  // "Forward declaration" of RegExp.prototype.
  // ES6: 21.2.5 "The RegExp prototype object is an ordinary object. It is not a
  // RegExp instance..."
  runtime->regExpPrototype =
      JSObject::create(
          runtime, Handle<JSObject>::vmcast(&runtime->objectPrototype))
          .getHermesValue();

  // "Forward declaration" of WeakMap.prototype.
  runtime->weakMapPrototype = JSObject::create(runtime).getHermesValue();

  // "Forward declaration" of WeakSet.prototype.
  runtime->weakSetPrototype = JSObject::create(runtime).getHermesValue();

  // "Forward declaration" of %ArrayIteratorPrototype%.
  runtime->arrayIteratorPrototype =
      JSObject::create(
          runtime, Handle<JSObject>::vmcast(&runtime->iteratorPrototype))
          .getHermesValue();

  // "Forward declaration" of %StringIteratorPrototype%.
  runtime->stringIteratorPrototype =
      JSObject::create(
          runtime, Handle<JSObject>::vmcast(&runtime->iteratorPrototype))
          .getHermesValue();

  runtime->generatorPrototype =
      JSObject::create(
          runtime, Handle<JSObject>::vmcast(&runtime->iteratorPrototype))
          .getHermesValue();

  // %Generator% intrinsic object.
  runtime->generatorFunctionPrototype =
      JSObject::create(
          runtime, Handle<JSObject>::vmcast(&runtime->functionPrototype))
          .getHermesValue();

  // Object constructor.
  createObjectConstructor(runtime);

  // JSError constructor.
  runtime->errorConstructor = createErrorConstructor(runtime).getHermesValue();

// All Native Error constructors.
#define NATIVE_ERROR_TYPE(name)       \
  create##name##Constructor(runtime); \
  gcScope.clearAllHandles();
#include "hermes/VM/NativeErrorTypes.def"

  // String constructor.
  createStringConstructor(runtime);

  // Function constructor.
  createFunctionConstructor(runtime);

  // Number constructor.
  createNumberConstructor(runtime);

  // Boolean constructor.
  createBooleanConstructor(runtime);

  // Date constructor.
  createDateConstructor(runtime);

  // RegExp constructor
  createRegExpConstructor(runtime);
  runtime->regExpLastInput = HermesValue::encodeUndefinedValue();
  runtime->regExpLastRegExp = HermesValue::encodeUndefinedValue();

  // Array constructor.
  createArrayConstructor(runtime);

  // ArrayBuffer constructor.
  createArrayBufferConstructor(runtime);

  // DataView constructor.
  createDataViewConstructor(runtime);

  // TypedArrayBase constructor.
  runtime->typedArrayBaseConstructor =
      createTypedArrayBaseConstructor(runtime).getHermesValue();

#define TYPED_ARRAY(name, type)                                             \
  runtime->name##ArrayConstructor =                                         \
      createTypedArrayConstructor<type, CellKind::name##ArrayKind>(runtime) \
          .getHermesValue();                                                \
  gcScope.clearAllHandles();
#include "hermes/VM/TypedArrays.def"

  // Set constructor.
  createSetConstructor(runtime);

  // Map constructor.
  createMapConstructor(runtime);

  // WeakMap constructor.
  createWeakMapConstructor(runtime);

  // WeakSet constructor.
  createWeakSetConstructor(runtime);

  // Symbol constructor.
  if (LLVM_UNLIKELY(runtime->hasES6Symbol())) {
    createSymbolConstructor(runtime);
  }

  /// %IteratorPrototype%.
  populateIteratorPrototype(runtime);

  /// Array Iterator.
  populateArrayIteratorPrototype(runtime);

  /// String Iterator.
  populateStringIteratorPrototype(runtime);

  // Define the global Math object
  runtime->ignoreAllocationFailure(JSObject::defineOwnProperty(
      runtime->getGlobal(),
      runtime,
      Predefined::getSymbolID(Predefined::Math),
      normalDPF,
      createMathObject(runtime)));

  // Define the global JSON object
  runtime->ignoreAllocationFailure(JSObject::defineOwnProperty(
      runtime->getGlobal(),
      runtime,
      Predefined::getSymbolID(Predefined::JSON),
      normalDPF,
      createJSONObject(runtime)));

  // Define the global %HermesInternal object.
  runtime->ignoreAllocationFailure(JSObject::defineOwnProperty(
      runtime->getGlobal(),
      runtime,
      Predefined::getSymbolID(Predefined::HermesInternal),
      constantDPF,
      createHermesInternalObject(runtime)));

#ifdef HERMES_ENABLE_DEBUGGER

  // Define the global %DebuggerInternal object.
  runtime->ignoreAllocationFailure(JSObject::defineOwnProperty(
      runtime->getGlobal(),
      runtime,
      Predefined::getSymbolID(Predefined::DebuggerInternal),
      constantDPF,
      createDebuggerInternalObject(runtime)));

#endif // HERMES_ENABLE_DEBUGGER

  // Define the 'print' function.
  defineGlobalFunc(Predefined::getSymbolID(Predefined::print), print, 1);

  // Define the 'eval' function.
  defineGlobalFunc(Predefined::getSymbolID(Predefined::eval), eval, 1);

  // Define the 'isNaN' function.
  defineGlobalFunc(Predefined::getSymbolID(Predefined::isNaN), isNaN, 1);

  // Define the 'isFinite' function.
  defineGlobalFunc(Predefined::getSymbolID(Predefined::isFinite), isFinite, 1);

  // Define the 'escape' function.
  defineGlobalFunc(Predefined::getSymbolID(Predefined::escape), escape, 1);

  // Define the 'unescape' function.
  defineGlobalFunc(Predefined::getSymbolID(Predefined::unescape), unescape, 1);

  // Define the 'decodeURI' function.
  defineGlobalFunc(
      Predefined::getSymbolID(Predefined::decodeURI), decodeURI, 1);

  // Define the 'decodeURIComponent' function.
  defineGlobalFunc(
      Predefined::getSymbolID(Predefined::decodeURIComponent),
      decodeURIComponent,
      1);

  // Define the 'encodeURI' function.
  defineGlobalFunc(
      Predefined::getSymbolID(Predefined::encodeURI), encodeURI, 1);

  // Define the 'encodeURIComponent' function.
  defineGlobalFunc(
      Predefined::getSymbolID(Predefined::encodeURIComponent),
      encodeURIComponent,
      1);

  // Define the 'gc' function.
  defineGlobalFunc(Predefined::getSymbolID(Predefined::gc), gc, 0);
}

} // namespace vm
} // namespace hermes
