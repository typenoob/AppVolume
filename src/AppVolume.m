#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#import "VolumePolicy.h"
#import "AudioQueueVolume.h"

static NSRecursiveLock *QAVLock;
static NSHashTable *QAVPlayers;
static float QAVGain = 1.0f;
static BOOL QAVEnabled = YES;
static NSString *QAVPreferenceKey;
static NSString *const QAVSettingsChanged = @"io.coyote.AppVolume.settingsChanged";
static NSUInteger QAVGeneration;
static char QAVStateKey;
static char QAVGestureKey;
static _Thread_local unsigned QAVInternalVolumeWrite;
static _Thread_local unsigned QAVTransportDepth;

typedef void (*QAVSetVolumeIMP)(id, SEL, float);
typedef float (*QAVGetVolumeIMP)(id, SEL);
typedef void (*QAVFadeVolumeIMP)(id, SEL, float, NSTimeInterval);
static QAVFadeVolumeIMP QAVAudioPlayerFade;

@interface QAVPlayerState : NSObject
@property(nonatomic) QAVVolumePolicy policy;
@property(nonatomic) QAVSetVolumeIMP setter;
@property(nonatomic) QAVFadeVolumeIMP fader;
@property(nonatomic) NSUInteger revision;
@end
@implementation QAVPlayerState
@end

static QAVPlayerState *QAVRegisterLocked(id player, QAVSetVolumeIMP setter, float base) {
    QAVPlayerState *state = objc_getAssociatedObject(player, &QAVStateKey);
    if (!state) {
        state = [QAVPlayerState new];
        state.policy = (QAVVolumePolicy){ .requestedVolume = base };
        state.setter = setter;
        if ([player isKindOfClass:AVAudioPlayer.class]) state.fader = QAVAudioPlayerFade;
        objc_setAssociatedObject(player, &QAVStateKey, state, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [QAVPlayers addObject:player];
    }
    return state;
}

static void QAVApplyLatest(id player) {
    for (;;) {
        [QAVLock lock];
        QAVPlayerState *state = objc_getAssociatedObject(player, &QAVStateKey);
        QAVVolumePolicy policy = state.policy;
        QAVSetVolumeIMP setter = state.setter;
        QAVFadeVolumeIMP fader = state.fader;
        NSUInteger revision = state.revision;
        NSUInteger generation = QAVGeneration;
        float gain = QAVGain;
        BOOL enabled = QAVEnabled;
        [QAVLock unlock];

        double now = NSProcessInfo.processInfo.systemUptime;
        QAVInternalVolumeWrite++;
        @try {
            setter(player, @selector(setVolume:), QAVScaleVolume(QAVRequestedVolumeAt(policy, now), gain, enabled));
            double remaining = QAVRemainingFade(policy, now);
            if (fader && remaining > 0.0) {
                fader(player, @selector(setVolume:fadeDuration:), QAVEffectiveVolume(policy, gain, enabled), remaining);
            }
        } @finally {
            QAVInternalVolumeWrite--;
        }

        // Native methods must run outside our lock; retry if a concurrent write superseded this snapshot.
        [QAVLock lock];
        BOOL current = revision == state.revision && generation == QAVGeneration;
        [QAVLock unlock];
        if (current) return;
    }
}

static void QAVUpdate(float gain, BOOL enabled) {
    gain = QAVNormalizeGain(gain);
    [QAVLock lock];
    QAVGain = gain;
    QAVEnabled = enabled;
    QAVGeneration++;
    NSArray *players = QAVPlayers.allObjects;
    [QAVLock unlock];
    QAVAudioQueueSetGain(gain, enabled);
    for (id player in players) QAVApplyLatest(player);
    [[NSUserDefaults standardUserDefaults] setObject:@{@"gain": @(gain), @"enabled": @(enabled)} forKey:QAVPreferenceKey];
    [NSNotificationCenter.defaultCenter postNotificationName:QAVSettingsChanged object:nil];
}

static void QAVPreparePlayback(id player, QAVSetVolumeIMP setter, QAVGetVolumeIMP getter) {
    [QAVLock lock];
    BOOL registered = objc_getAssociatedObject(player, &QAVStateKey) != nil;
    [QAVLock unlock];
    if (registered) return;
    float base = getter(player, @selector(volume));
    [QAVLock lock];
    QAVRegisterLocked(player, setter, base);
    [QAVLock unlock];
    QAVApplyLatest(player);
}

static void QAVReplace(Class cls, SEL selector, IMP replacement) {
    Method method = class_getInstanceMethod(cls, selector);
    if (!class_addMethod(cls, selector, replacement, method_getTypeEncoding(method))) {
        method_setImplementation(class_getInstanceMethod(cls, selector), replacement);
    }
}

static void QAVHookVoidTransport(Class cls, SEL selector, QAVSetVolumeIMP setter, QAVGetVolumeIMP getter) {
    Method method = class_getInstanceMethod(cls, selector);
    if (!method) return;
    void (*original)(id, SEL) = (void *)method_getImplementation(method);
    QAVReplace(cls, selector, imp_implementationWithBlock(^(id player) {
        if (!QAVTransportDepth) QAVPreparePlayback(player, setter, getter);
        QAVTransportDepth++;
        @try { original(player, selector); }
        @finally { QAVTransportDepth--; }
    }));
}

static void QAVHookObjectTransport(Class cls, SEL selector, QAVSetVolumeIMP setter, QAVGetVolumeIMP getter) {
    Method method = class_getInstanceMethod(cls, selector);
    if (!method) return;
    void (*original)(id, SEL, id) = (void *)method_getImplementation(method);
    QAVReplace(cls, selector, imp_implementationWithBlock(^(id player, id argument) {
        if (!QAVTransportDepth) QAVPreparePlayback(player, setter, getter);
        QAVTransportDepth++;
        @try { original(player, selector, argument); }
        @finally { QAVTransportDepth--; }
    }));
}

static void QAVHookClass(NSString *name) {
    Class cls = NSClassFromString(name);
    Method setMethod = class_getInstanceMethod(cls, @selector(setVolume:));
    Method getMethod = class_getInstanceMethod(cls, @selector(volume));
    if (!setMethod || !getMethod) return;
    QAVSetVolumeIMP setter = (void *)method_getImplementation(setMethod);
    QAVGetVolumeIMP getter = (void *)method_getImplementation(getMethod);
    if ([name isEqualToString:@"AVAudioPlayer"]) {
        QAVAudioPlayerFade = (void *)class_getMethodImplementation(cls, @selector(setVolume:fadeDuration:));
    }

    QAVReplace(cls, @selector(setVolume:), imp_implementationWithBlock(^(id player, float volume) {
        if (QAVInternalVolumeWrite) {
            setter(player, @selector(setVolume:), volume);
            return;
        }
        [QAVLock lock];
        QAVPlayerState *state = QAVRegisterLocked(player, setter, volume);
        state.policy = (QAVVolumePolicy){ .requestedVolume = volume };
        state.revision++;
        [QAVLock unlock];
        QAVApplyLatest(player);
    }));

    QAVReplace(cls, @selector(volume), imp_implementationWithBlock(^float(id player) {
        if (QAVInternalVolumeWrite) return getter(player, @selector(volume));
        [QAVLock lock];
        QAVPlayerState *state = objc_getAssociatedObject(player, &QAVStateKey);
        QAVVolumePolicy policy = state.policy;
        [QAVLock unlock];
        return state ? QAVRequestedVolumeAt(policy, NSProcessInfo.processInfo.systemUptime) : getter(player, @selector(volume));
    }));

    if ([name isEqualToString:@"AVAudioPlayer"]) {
        SEL play = @selector(play);
        BOOL (*originalPlay)(id, SEL) = (void *)class_getMethodImplementation(cls, play);
        QAVReplace(cls, play, imp_implementationWithBlock(^BOOL(id player) {
            if (!QAVTransportDepth) QAVPreparePlayback(player, setter, getter);
            QAVTransportDepth++;
            @try { return originalPlay(player, play); }
            @finally { QAVTransportDepth--; }
        }));
        SEL timedPlay = @selector(playAtTime:);
        BOOL (*originalTimedPlay)(id, SEL, NSTimeInterval) = (void *)class_getMethodImplementation(cls, timedPlay);
        QAVReplace(cls, timedPlay, imp_implementationWithBlock(^BOOL(id player, NSTimeInterval time) {
            if (!QAVTransportDepth) QAVPreparePlayback(player, setter, getter);
            QAVTransportDepth++;
            @try { return originalTimedPlay(player, timedPlay, time); }
            @finally { QAVTransportDepth--; }
        }));
        SEL fade = @selector(setVolume:fadeDuration:);
        QAVFadeVolumeIMP originalFade = QAVAudioPlayerFade;
        QAVReplace(cls, fade, imp_implementationWithBlock(^(id player, float volume, NSTimeInterval duration) {
            if (QAVInternalVolumeWrite) {
                originalFade(player, fade, volume, duration);
                return;
            }
            QAVPreparePlayback(player, setter, getter);
            double now = NSProcessInfo.processInfo.systemUptime;
            [QAVLock lock];
            QAVPlayerState *state = objc_getAssociatedObject(player, &QAVStateKey);
            state.policy = QAVBeginFade(state.policy, volume, duration, now);
            state.revision++;
            [QAVLock unlock];
            QAVApplyLatest(player);
        }));
    } else if ([name isEqualToString:@"AVSampleBufferAudioRenderer"]) {
        SEL enqueue = @selector(enqueueSampleBuffer:);
        void (*original)(id, SEL, CMSampleBufferRef) = (void *)class_getMethodImplementation(cls, enqueue);
        QAVReplace(cls, enqueue, imp_implementationWithBlock(^(id player, CMSampleBufferRef buffer) {
            if (!QAVTransportDepth) QAVPreparePlayback(player, setter, getter);
            QAVTransportDepth++;
            @try { original(player, enqueue, buffer); }
            @finally { QAVTransportDepth--; }
        }));
    } else {
        QAVHookVoidTransport(cls, @selector(play), setter, getter);
        if ([name isEqualToString:@"AVAudioPlayerNode"]) {
            QAVHookObjectTransport(cls, @selector(playAtTime:), setter, getter);
        } else {
            SEL timedRate = @selector(setRate:time:atHostTime:);
            Method timedMethod = class_getInstanceMethod(cls, timedRate);
            if (timedMethod) {
                void (*originalTimedRate)(id, SEL, float, CMTime, CMTime) = (void *)method_getImplementation(timedMethod);
                QAVReplace(cls, timedRate, imp_implementationWithBlock(^(id player, float rate, CMTime time, CMTime hostTime) {
                    if (rate != 0.0f && !QAVTransportDepth) QAVPreparePlayback(player, setter, getter);
                    QAVTransportDepth++;
                    @try { originalTimedRate(player, timedRate, rate, time, hostTime); }
                    @finally { QAVTransportDepth--; }
                }));
            }
            for (NSString *selectorName in @[@"setRate:", @"playImmediatelyAtRate:"]) {
                SEL selector = NSSelectorFromString(selectorName);
                Method method = class_getInstanceMethod(cls, selector);
                if (!method) continue;
                void (*original)(id, SEL, float) = (void *)method_getImplementation(method);
                QAVReplace(cls, selector, imp_implementationWithBlock(^(id player, float rate) {
                    if (rate != 0.0f && !QAVTransportDepth) QAVPreparePlayback(player, setter, getter);
                    QAVTransportDepth++;
                    @try { original(player, selector, rate); }
                    @finally { QAVTransportDepth--; }
                }));
            }
        }
    }
}

@interface QAVPanelController : UIViewController
@property(nonatomic, strong) UILabel *percentage;
@property(nonatomic, strong) UILabel *diagnostics;
@property(nonatomic, strong) UISlider *slider;
@property(nonatomic, strong) UISwitch *enabledSwitch;
@property(nonatomic, strong) NSTimer *diagnosticsTimer;
@end

@implementation QAVPanelController
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    UILabel *title = [UILabel new];
    title.text = @"App 独立音量 · 0.2";
    title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle2];
    UILabel *app = [UILabel new];
    app.text = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleDisplayName"] ?: NSBundle.mainBundle.bundleIdentifier;
    app.textColor = UIColor.secondaryLabelColor;
    app.numberOfLines = 2;
    self.percentage = [UILabel new];
    self.percentage.font = [UIFont monospacedDigitSystemFontOfSize:32 weight:UIFontWeightMedium];
    self.slider = [UISlider new];
    self.slider.minimumValue = 0.0f;
    self.slider.maximumValue = 1.0f;
    [self.slider addTarget:self action:@selector(changeVolume:) forControlEvents:UIControlEventValueChanged];
    UILabel *enabledLabel = [UILabel new];
    enabledLabel.text = @"启用独立音量";
    self.enabledSwitch = [UISwitch new];
    [self.enabledSwitch addTarget:self action:@selector(changeEnabled:) forControlEvents:UIControlEventValueChanged];
    UIStackView *toggleRow = [[UIStackView alloc] initWithArrangedSubviews:@[enabledLabel, self.enabledSwitch]];
    toggleRow.axis = UILayoutConstraintAxisHorizontal;
    toggleRow.alignment = UIStackViewAlignmentCenter;
    self.diagnostics = [UILabel new];
    self.diagnostics.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    self.diagnostics.textColor = UIColor.secondaryLabelColor;
    self.diagnostics.numberOfLines = 0;
    UILabel *help = [UILabel new];
    help.text = @"三指双击打开此面板。设置仅保存在当前 App 内；100% 保留播放器原音量，0% 静音。不改变系统音量或后台播放策略。";
    help.numberOfLines = 0;
    help.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    help.textColor = UIColor.secondaryLabelColor;
    UIButton *reset = [UIButton buttonWithType:UIButtonTypeSystem];
    [reset setTitle:@"恢复 100%" forState:UIControlStateNormal];
    [reset addTarget:self action:@selector(resetVolume) forControlEvents:UIControlEventTouchUpInside];
    UIButton *close = [UIButton buttonWithType:UIButtonTypeSystem];
    [close setTitle:@"完成" forState:UIControlStateNormal];
    [close addTarget:self action:@selector(closePanel) forControlEvents:UIControlEventTouchUpInside];
    UIStackView *buttons = [[UIStackView alloc] initWithArrangedSubviews:@[reset, close]];
    buttons.distribution = UIStackViewDistributionFillEqually;
    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[title, app, self.percentage, self.slider, toggleRow, self.diagnostics, help, buttons]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 16;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    UIScrollView *scroll = [UIScrollView new];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:scroll];
    [scroll addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:24],
        [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor constant:-24],
        [stack.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor constant:24],
        [stack.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor constant:-24],
        [stack.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor constant:-48]
    ]];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(settingsChanged:) name:QAVSettingsChanged object:nil];
    [self refresh];
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    __weak QAVPanelController *weakSelf = self;
    self.diagnosticsTimer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *timer) {
        [weakSelf refreshDiagnostics];
    }];
}
- (void)viewDidDisappear:(BOOL)animated {
    [self.diagnosticsTimer invalidate];
    self.diagnosticsTimer = nil;
    [super viewDidDisappear:animated];
}
- (void)settingsChanged:(NSNotification *)notification {
    [self refresh];
}
- (void)refreshDiagnostics {
    [QAVLock lock];
    NSUInteger count = QAVPlayers.allObjects.count;
    [QAVLock unlock];
    QAVAudioQueueDiagnostics queues = QAVAudioQueueGetDiagnostics();
    self.diagnostics.text = [NSString stringWithFormat:@"AVFoundation：%lu 个\nAudioQueue：%llu 个（累计 %llu）\n队列音量应用：%llu 次，状态：%d\n已扫描应用模块：%llu\nAudioQueue 设置在下一次送入音频缓冲时生效。", (unsigned long)count, (unsigned long long)queues.activeQueues, (unsigned long long)queues.createdQueues, (unsigned long long)queues.appliedChanges, queues.lastError, (unsigned long long)queues.scannedImages];
}
- (void)refresh {
    [QAVLock lock];
    float gain = QAVGain;
    BOOL enabled = QAVEnabled;
    [QAVLock unlock];
    self.slider.value = gain;
    self.slider.enabled = enabled;
    self.enabledSwitch.on = enabled;
    self.percentage.text = [NSString stringWithFormat:@"%.0f%%%@", gain * 100.0f, enabled ? @"" : @"（已停用）"];
    [self refreshDiagnostics];
}
- (void)changeVolume:(UISlider *)slider {
    QAVUpdate(slider.value, self.enabledSwitch.on);
    [self refresh];
}
- (void)changeEnabled:(UISwitch *)sender {
    QAVUpdate(self.slider.value, sender.on);
    [self refresh];
}
- (void)resetVolume {
    QAVUpdate(1.0f, YES);
    [self refresh];
}
- (void)closePanel {
    [self dismissViewControllerAnimated:YES completion:nil];
}
@end

@interface QAVUIManager : NSObject <UIGestureRecognizerDelegate>
- (void)attachWindows;
@end

@implementation QAVUIManager
- (instancetype)init {
    self = [super init];
    if (self) {
        NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
        [center addObserver:self selector:@selector(windowVisible:) name:UIWindowDidBecomeVisibleNotification object:nil];
        [center addObserver:self selector:@selector(applicationActive:) name:UIApplicationDidBecomeActiveNotification object:nil];
        [center addObserver:self selector:@selector(applicationActive:) name:UISceneDidActivateNotification object:nil];
    }
    return self;
}
- (void)attachWindow:(UIWindow *)window {
    [self attachWindow:window attemptsRemaining:4];
}
- (void)attachWindow:(UIWindow *)window attemptsRemaining:(NSUInteger)attempts {
    if (!window || window.windowLevel != UIWindowLevelNormal || objc_getAssociatedObject(window, &QAVGestureKey)) return;
    if (!window.rootViewController) {
        if (attempts > 0) {
            __weak UIWindow *weakWindow = window;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
                [self attachWindow:weakWindow attemptsRemaining:attempts - 1];
            });
        }
        return;
    }
    UITapGestureRecognizer *gesture = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(openPanel:)];
    gesture.numberOfTouchesRequired = 3;
    gesture.numberOfTapsRequired = 2;
    gesture.cancelsTouchesInView = NO;
    gesture.delaysTouchesBegan = NO;
    gesture.delaysTouchesEnded = NO;
    gesture.delegate = self;
    [window addGestureRecognizer:gesture];
    objc_setAssociatedObject(window, &QAVGestureKey, gesture, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
- (void)attachWindows {
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if ([scene isKindOfClass:UIWindowScene.class]) {
            for (UIWindow *window in ((UIWindowScene *)scene).windows) [self attachWindow:window];
        }
    }
    for (UIWindow *window in UIApplication.sharedApplication.windows) [self attachWindow:window];
}
- (void)windowVisible:(NSNotification *)notification {
    dispatch_async(dispatch_get_main_queue(), ^{ [self attachWindow:notification.object]; });
}
- (void)applicationActive:(NSNotification *)notification {
    [self attachWindows];
}
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)gesture shouldRecognizeSimultaneouslyWithGestureRecognizer:(UIGestureRecognizer *)other {
    return YES;
}
- (void)openPanel:(UITapGestureRecognizer *)gesture {
    if (gesture.state != UIGestureRecognizerStateRecognized) return;
    UIViewController *presenter = ((UIWindow *)gesture.view).rootViewController;
    while (presenter.presentedViewController) presenter = presenter.presentedViewController;
    if (!presenter || [presenter isKindOfClass:QAVPanelController.class] || presenter.isBeingDismissed || presenter.isBeingPresented) return;
    QAVPanelController *panel = [QAVPanelController new];
    panel.modalPresentationStyle = UIModalPresentationPageSheet;
    panel.sheetPresentationController.detents = @[UISheetPresentationControllerDetent.largeDetent];
    panel.sheetPresentationController.prefersGrabberVisible = YES;
    [presenter presentViewController:panel animated:YES completion:nil];
}
@end

__attribute__((constructor)) static void QAVInitialize(void) {
    @autoreleasepool {
        NSBundle *bundle = NSBundle.mainBundle;
        if ([bundle.bundleIdentifier isEqualToString:@"com.apple.springboard"] || [bundle.bundlePath.pathExtension isEqualToString:@"appex"]) return;
        QAVLock = [NSRecursiveLock new];
        QAVPlayers = [NSHashTable weakObjectsHashTable];
        QAVPreferenceKey = [@"io.coyote.AppVolume.settings." stringByAppendingString:bundle.bundleIdentifier ?: @"main"];
        id saved = [[NSUserDefaults standardUserDefaults] objectForKey:QAVPreferenceKey];
        if ([saved isKindOfClass:NSDictionary.class]) {
            id gain = saved[@"gain"];
            id enabled = saved[@"enabled"];
            if ([gain isKindOfClass:NSNumber.class]) QAVGain = QAVNormalizeGain([gain floatValue]);
            if ([enabled isKindOfClass:NSNumber.class]) QAVEnabled = [enabled boolValue];
        }
        for (NSString *name in @[@"AVPlayer", @"AVAudioPlayer", @"AVAudioPlayerNode", @"AVSampleBufferAudioRenderer"]) {
            QAVHookClass(name);
        }
        QAVAudioQueueInstall(bundle.bundlePath.fileSystemRepresentation, QAVGain, QAVEnabled);
        dispatch_async(dispatch_get_main_queue(), ^{
            static QAVUIManager *manager;
            manager = [QAVUIManager new];
            [manager attachWindows];
        });
    }
}
