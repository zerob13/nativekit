#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include "overlay/overlay_manager.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "common/mac/image_utils.h"

@interface NativekitOverlayPanel : NSPanel
@end

@implementation NativekitOverlayPanel
- (BOOL)canBecomeKeyWindow {
  return NO;
}
- (BOOL)canBecomeMainWindow {
  return NO;
}
@end

@interface NativekitOverlayView : NSView
- (instancetype)initWithActivate:(std::function<void()>)activate
                             hide:(std::function<void()>)hide
                         relocate:(std::function<void()>)relocate;
- (void)setContentImage:(NSImage*)image
               iconPath:(NSString*)iconPath
            hideTooltip:(NSString*)hideTooltip
        relocateTooltip:(NSString*)relocateTooltip;
@end

@implementation NativekitOverlayView {
  NSImageView* image_view_;
  NSImageView* icon_view_;
  NSButton* hide_button_;
  NSButton* relocate_button_;
  std::function<void()> activate_;
  std::function<void()> hide_;
  std::function<void()> relocate_;
}

- (instancetype)initWithActivate:(std::function<void()>)activate
                             hide:(std::function<void()>)hide
                         relocate:(std::function<void()>)relocate {
  self = [super initWithFrame:NSZeroRect];
  if (self == nil) return nil;

  activate_ = std::move(activate);
  hide_ = std::move(hide);
  relocate_ = std::move(relocate);
  self.wantsLayer = YES;
  self.layer.backgroundColor = NSColor.clearColor.CGColor;

  image_view_ = [[NSImageView alloc] initWithFrame:NSZeroRect];
  image_view_.imageScaling = NSImageScaleProportionallyUpOrDown;
  image_view_.imageAlignment = NSImageAlignCenter;
  image_view_.accessibilityElement = YES;
  [self addSubview:image_view_];

  icon_view_ = [[NSImageView alloc] initWithFrame:NSZeroRect];
  icon_view_.imageScaling = NSImageScaleProportionallyUpOrDown;
  icon_view_.wantsLayer = YES;
  icon_view_.layer.cornerRadius = 6;
  icon_view_.layer.masksToBounds = YES;
  [self addSubview:icon_view_];

  hide_button_ = [NSButton buttonWithImage:
      [NSImage imageWithSystemSymbolName:@"eye.slash"
                accessibilityDescription:@"Hide overlay"]
                                target:self
                                action:@selector(hideOverlay:)];
  relocate_button_ = [NSButton buttonWithImage:
      [NSImage imageWithSystemSymbolName:@"arrow.triangle.2.circlepath"
                accessibilityDescription:@"Move overlay"]
                                    target:self
                                    action:@selector(relocateOverlay:)];
  for (NSButton* button in @[ hide_button_, relocate_button_ ]) {
    button.bezelStyle = NSBezelStyleCircular;
    button.bordered = YES;
    button.focusRingType = NSFocusRingTypeNone;
    button.contentTintColor = NSColor.labelColor;
    [self addSubview:button];
  }

  NSClickGestureRecognizer* double_click = [[NSClickGestureRecognizer alloc]
      initWithTarget:self
              action:@selector(activateOverlay:)];
  double_click.numberOfClicksRequired = 2;
  [self addGestureRecognizer:double_click];
  return self;
}

- (BOOL)acceptsFirstMouse:(NSEvent*)event {
  return YES;
}

- (void)layout {
  [super layout];
  image_view_.frame = self.bounds;
  constexpr CGFloat button_size = 26;
  constexpr CGFloat margin = 8;
  hide_button_.frame = NSMakeRect(
      NSMaxX(self.bounds) - margin - button_size,
      NSMaxY(self.bounds) - margin - button_size,
      button_size,
      button_size);
  relocate_button_.frame = NSMakeRect(
      NSMinX(hide_button_.frame) - 6 - button_size,
      NSMinY(hide_button_.frame),
      button_size,
      button_size);
  icon_view_.frame = NSMakeRect(margin, margin, 28, 28);
}

- (void)setContentImage:(NSImage*)image
               iconPath:(NSString*)iconPath
            hideTooltip:(NSString*)hideTooltip
        relocateTooltip:(NSString*)relocateTooltip {
  image_view_.image = image;
  image_view_.accessibilityLabel = @"Overlay image";
  icon_view_.image = iconPath.length == 0
                         ? nil
                         : [NSWorkspace.sharedWorkspace iconForFile:iconPath];
  icon_view_.hidden = icon_view_.image == nil;
  hide_button_.toolTip = hideTooltip;
  relocate_button_.toolTip = relocateTooltip;
}

- (void)activateOverlay:(id)sender {
  if (activate_) activate_();
}

- (void)hideOverlay:(id)sender {
  if (hide_) hide_();
}

- (void)relocateOverlay:(id)sender {
  if (relocate_) relocate_();
}

@end

namespace nativekit::platform {
namespace {

NSString* ns_string(const std::string& value) {
  return [NSString stringWithUTF8String:value.c_str()] ?: @"";
}

NSSize fitted_size(NSImage* image, const OverlayHost& host, double max_size) {
  NSSize size = image.size;
  if (size.width <= 0 || size.height <= 0) {
    size = NSMakeSize(host.bounds.width, host.bounds.height);
  }
  const double width_limit =
      std::min(max_size, std::max(host.bounds.width, 64.0));
  const double height_limit =
      std::min(max_size, std::max(host.bounds.height, 64.0));
  const double scale = std::min({
      1.0,
      width_limit / std::max<double>(size.width, 1),
      height_limit / std::max<double>(size.height, 1),
      max_size / std::max<double>(std::max(size.width, size.height), 1),
  });
  return NSMakeSize(
      std::max<CGFloat>(64, std::floor(size.width * scale)),
      std::max<CGFloat>(64, std::floor(size.height * scale)));
}

NSScreen* screen_for_host(const OverlayHost& host) {
  NSView* view = (__bridge NSView*)(reinterpret_cast<void*>(host.window_handle));
  return view.window.screen ?: NSScreen.mainScreen ?: NSScreen.screens.firstObject;
}

NSRect frame_for_presentation(
    const OverlayHost& host,
    NSSize size,
    double cursor) {
  NSScreen* screen = screen_for_host(host);
  if (screen == nil) return NSMakeRect(0, 0, size.width, size.height);
  const NSRect frame = screen.visibleFrame;
  const CGFloat offset = host.anchor.offset;
  switch (host.anchor.edge) {
    case AnchorEdge::kLeading:
      return NSMakeRect(
          NSMinX(frame) + offset,
          NSMaxY(frame) - offset - cursor - size.height,
          size.width,
          size.height);
    case AnchorEdge::kTrailing:
      return NSMakeRect(
          NSMaxX(frame) - offset - size.width,
          NSMaxY(frame) - offset - cursor - size.height,
          size.width,
          size.height);
    case AnchorEdge::kTop:
      return NSMakeRect(
          NSMinX(frame) + offset + cursor,
          NSMaxY(frame) - offset - size.height,
          size.width,
          size.height);
    case AnchorEdge::kBottom:
      return NSMakeRect(
          NSMinX(frame) + offset + cursor,
          NSMinY(frame) + offset,
          size.width,
          size.height);
  }
  return NSMakeRect(0, 0, size.width, size.height);
}

bool presentation_fits(
    const OverlayHost& host,
    NSSize size,
    double cursor) {
  NSScreen* screen = screen_for_host(host);
  if (screen == nil) return false;
  const NSSize available = screen.visibleFrame.size;
  const double offset = host.anchor.offset;
  if (host.anchor.edge == AnchorEdge::kLeading ||
      host.anchor.edge == AnchorEdge::kTrailing) {
    return offset + size.width <= available.width &&
           offset + cursor + size.height <= available.height;
  }
  return offset + size.height <= available.height &&
         offset + cursor + size.width <= available.width;
}

class MacOverlayPlatform final : public OverlayPlatform {
 public:
  explicit MacOverlayPlatform(OverlayPlatformEvents events)
      : events_(std::move(events)),
        panels_([[NSMutableDictionary alloc] init]) {}

  ~MacOverlayPlatform() override { stop(); }

  void update(const OverlaySnapshot& snapshot) override {
    require_main_thread();

    std::unordered_map<std::string, OverlayHost> hosts;
    for (const auto& host : snapshot.hosts) hosts.emplace(host.id, host);

    std::unordered_set<std::string> active_ids;
    std::unordered_map<std::string, double> cursors;
    for (const auto& presentation : snapshot.presentations) {
      active_ids.insert(presentation.id);
      const auto host = hosts.find(presentation.host_id);
      if (host == hosts.end()) continue;

      NSString* identifier = ns_string(presentation.id);
      NativekitOverlayPanel* panel = panels_[identifier];
      const bool created = panel == nil;
      if (created) {
        panel = [[NativekitOverlayPanel alloc]
            initWithContentRect:NSMakeRect(0, 0, 64, 64)
                      styleMask:NSWindowStyleMaskBorderless |
                                NSWindowStyleMaskNonactivatingPanel
                        backing:NSBackingStoreBuffered
                          defer:NO];
        panel.level = NSFloatingWindowLevel;
        panel.opaque = NO;
        panel.backgroundColor = NSColor.clearColor;
        panel.hasShadow = YES;
        panel.hidesOnDeactivate = NO;
        panel.releasedWhenClosed = NO;
        panel.becomesKeyOnlyIfNeeded = YES;
        panel.collectionBehavior =
            NSWindowCollectionBehaviorCanJoinAllSpaces |
            NSWindowCollectionBehaviorStationary |
            NSWindowCollectionBehaviorIgnoresCycle |
            NSWindowCollectionBehaviorFullScreenAuxiliary;
        const std::string host_id = presentation.host_id;
        panel.contentView = [[NativekitOverlayView alloc]
            initWithActivate:events_.activate
                         hide:[this] {
                           if (events_.visibility_request) {
                             events_.visibility_request(false);
                           }
                         }
                     relocate:[this, host_id] {
                       if (events_.relocate) events_.relocate(host_id);
                     }];
        panels_[identifier] = panel;
      }

      NSImage* image = image_from_data_url(presentation.image_data);
      if (image == nil) {
        throw std::runtime_error("overlay image data could not be decoded");
      }
      auto* view = (NativekitOverlayView*)panel.contentView;
      NSString* icon_path = presentation.app_icon_path
                                ? ns_string(*presentation.app_icon_path)
                                : nil;
      [view setContentImage:image
                   iconPath:icon_path
                hideTooltip:ns_string(snapshot.options.hide_tooltip)
            relocateTooltip:ns_string(snapshot.options.relocate_tooltip)];
      panel.title = ns_string(host->second.title);

      const NSSize size = fitted_size(image, host->second, snapshot.max_size);
      double& cursor = cursors[presentation.host_id];
      if (presentation.visible && snapshot.visible &&
          presentation_fits(host->second, size, cursor)) {
        const NSRect frame = frame_for_presentation(host->second, size, cursor);
        [panel setFrame:frame display:YES animate:NO];
        cursor += (host->second.anchor.edge == AnchorEdge::kLeading ||
                   host->second.anchor.edge == AnchorEdge::kTrailing)
                      ? size.height + 12
                      : size.width + 12;
        // Visible panels are ordered after layout so the active session wins.
      } else {
        [panel orderOut:nil];
      }
    }

    for (auto iterator = snapshot.presentations.rbegin();
         iterator != snapshot.presentations.rend(); ++iterator) {
      if (!iterator->visible || !snapshot.visible) continue;
      NativekitOverlayPanel* panel = panels_[ns_string(iterator->id)];
      if (panel != nil) [panel orderFrontRegardless];
    }

    for (NSString* identifier in panels_.allKeys.copy) {
      if (active_ids.find(identifier.UTF8String ?: "") != active_ids.end()) {
        continue;
      }
      NativekitOverlayPanel* panel = panels_[identifier];
      [panel orderOut:nil];
      [panel close];
      [panels_ removeObjectForKey:identifier];
    }
  }

  void stop() override {
    if (panels_ == nil) return;
    if (![NSThread isMainThread]) {
      auto* platform = this;
      dispatch_sync(dispatch_get_main_queue(), ^{
        platform->close_all();
      });
    } else {
      close_all();
    }
  }

 private:
  static void require_main_thread() {
    if (![NSThread isMainThread]) {
      throw std::runtime_error("overlay methods must run on the main thread");
    }
  }

  void close_all() {
    for (NativekitOverlayPanel* panel in panels_.allValues.copy) {
      [panel orderOut:nil];
      [panel close];
    }
    [panels_ removeAllObjects];
  }

  OverlayPlatformEvents events_;
  NSMutableDictionary<NSString*, NativekitOverlayPanel*>* panels_;
};

}  // namespace

std::unique_ptr<OverlayPlatform> create_overlay_platform(
    OverlayPlatformEvents events) {
  return std::make_unique<MacOverlayPlatform>(std::move(events));
}

}  // namespace nativekit::platform
