import Foundation

print("glucose-ring iOS core tests (protocol / Nightscout / relay rules)")
ringProtocolTests()
nightscoutTests()
relayTests()

let args = CommandLine.arguments
if let i = args.firstIndex(of: "--export-wire"), i + 1 < args.count {
    do {
        let n = try exportWire(to: URL(fileURLWithPath: args[i + 1]))
        print("\nexported \(n) wire lines to \(args[i + 1])")
    } catch {
        print("export failed: \(error)")
        failedCount += 1
    }
}

print("\n=====================================")
print("passed: \(passedCount)   failed: \(failedCount)")
print("=====================================")
exit(failedCount == 0 ? 0 : 1)
