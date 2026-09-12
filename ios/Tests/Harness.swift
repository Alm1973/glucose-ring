import Foundation

// A tiny test harness: XCTest is not in Command Line Tools. Same shape as
// tests/test_all.cpp -- count, print each failure, exit non-zero on any.

var passedCount = 0
var failedCount = 0
var currentSection = ""

func section(_ name: String) {
    currentSection = name
    print("\n-- \(name)")
}

func check(_ cond: Bool, _ what: @autoclosure () -> String,
           file: StaticString = #fileID, line: UInt = #line) {
    if cond {
        passedCount += 1
    } else {
        failedCount += 1
        print("  FAIL [\(currentSection)] \(what()) (\(file):\(line))")
    }
}

func expectEqual<T: Equatable>(_ got: T, _ want: T, _ what: String,
                               file: StaticString = #fileID, line: UInt = #line) {
    check(got == want, "\(what): got \(got), want \(want)", file: file, line: line)
}
