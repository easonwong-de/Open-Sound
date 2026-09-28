import SwiftUI

/// Main application entrypoint for the OpenSound audio router.
@main
struct OpenSound: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
        }
        .windowResizability(.contentSize)
        .defaultSize(width: 580, height: 480)
    }
}
