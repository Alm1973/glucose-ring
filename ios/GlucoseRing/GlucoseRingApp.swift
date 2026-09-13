import Combine
import SwiftUI
import UIKit

/// Creates the relay at launch -- including a background relaunch by state
/// restoration, when no window may ever appear. The CBCentralManager must
/// exist, with its restore identifier, before didFinishLaunching returns.
final class AppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
        _ = RelayModel.shared
        return true
    }
}

@main
struct GlucoseRingApp: App {
    @UIApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate

    var body: some Scene {
        WindowGroup { StatusView(model: RelayModel.shared) }
    }
}

struct StatusView: View {
    @ObservedObject var model: RelayModel
    private let tick = Timer.publish(every: 30, on: .main, in: .common).autoconnect()

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                Text(model.status.title)
                    .font(.title2.bold())
                    .foregroundStyle(model.status.ok ? Color.primary : Color.red)
                Text(model.status.detail)
                Divider()
                Text(model.linkText)
                    .foregroundStyle(.secondary)
                Button("Find ring") { model.findRing() }
                    .buttonStyle(.borderedProminent)
                Text("Leave this app in the app switcher. If you swipe it away, iOS stops waking it for the "
                     + "ring, and the ring buzzes MISS about 15 minutes later. This is a secondary alert: keep "
                     + "your CGM app's own alarms on.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
            .padding()
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .onReceive(tick) { _ in model.refresh() }
    }
}
