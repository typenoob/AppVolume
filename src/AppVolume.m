#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>
#import <limits.h>
#import <objc/runtime.h>
#import "VolumePolicy.h"
#import "AudioQueueVolume.h"
#import "AudioUnitVolume.h"

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

// One class' volume accessors. AVAudioMixerNode uses outputVolume, the rest use volume.
typedef struct {
    QAVSetVolumeIMP setter;
    QAVGetVolumeIMP getter;
    SEL setSelector;
    SEL getSelector;
    // Engine nodes route through the mixer, which already carries the gain.
    BOOL defersToMixer;
} QAVVolumeHooks;

static QAVVolumeHooks QAVMixerHooks;
static BOOL QAVMixerHooksAvailable;
static BOOL QAVMixerRegistered;

// Every IMP this dylib installs, so a subclass that merely inherits one of them is not wrapped a second time and
// asked to scale the same volume twice.
static NSMutableSet *QAVInstalledIMPs;
static NSMutableDictionary<NSString *, NSNumber *> *QAVSetterHits;
static NSUInteger QAVMixerWrites;
static float QAVMixerRequested;
static float QAVMixerApplied;
static NSString *QAVMixerClassName;

@interface QAVPlayerState : NSObject
@property(nonatomic) QAVVolumePolicy policy;
@property(nonatomic) QAVVolumeHooks hooks;
@property(nonatomic) QAVFadeVolumeIMP fader;
@property(nonatomic) NSUInteger revision;
@end
@implementation QAVPlayerState
@end

static QAVPlayerState *QAVRegisterLocked(id player, QAVVolumeHooks hooks, float base) {
    QAVPlayerState *state = objc_getAssociatedObject(player, &QAVStateKey);
    if (!state) {
        state = [QAVPlayerState new];
        state.policy = (QAVVolumePolicy){ .requestedVolume = base };
        state.hooks = hooks;
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
        QAVVolumeHooks hooks = state.hooks;
        QAVFadeVolumeIMP fader = state.fader;
        NSUInteger revision = state.revision;
        NSUInteger generation = QAVGeneration;
        float gain = QAVGain;
        BOOL enabled = QAVEnabled;
        BOOL defers = hooks.defersToMixer && QAVMixerRegistered;
        [QAVLock unlock];
        if (!state) return;

        double now = NSProcessInfo.processInfo.systemUptime;
        double remaining = QAVRemainingFade(policy, now);
        QAVInternalVolumeWrite++;
        @try {
            if (defers) {
                // The mixer applies the gain for the whole graph; writing it twice would square it.
                hooks.setter(player, hooks.setSelector, QAVRequestedVolumeAt(policy, now));
            } else {
                hooks.setter(player, hooks.setSelector, QAVScaleVolume(QAVRequestedVolumeAt(policy, now), gain, enabled));
                if (fader && remaining > 0.0) {
                    fader(player, @selector(setVolume:fadeDuration:), QAVEffectiveVolume(policy, gain, enabled), remaining);
                }
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

static void QAVReapplyAll(void) {
    [QAVLock lock];
    NSArray *players = QAVPlayers.allObjects;
    [QAVLock unlock];
    for (id player in players) QAVApplyLatest(player);
}

static void QAVUpdate(float gain, BOOL enabled) {
    gain = QAVNormalizeGain(gain);
    [QAVLock lock];
    QAVGain = gain;
    QAVEnabled = enabled;
    QAVGeneration++;
    [QAVLock unlock];
    QAVAudioQueueSetGain(gain, enabled);
    QAVAudioUnitSetGain(gain, enabled);
    QAVReapplyAll();
    [[NSUserDefaults standardUserDefaults] setObject:@{@"gain": @(gain), @"enabled": @(enabled)} forKey:QAVPreferenceKey];
    [NSNotificationCenter.defaultCenter postNotificationName:QAVSettingsChanged object:nil];
}

static void QAVPreparePlayback(id player, QAVVolumeHooks hooks) {
    [QAVLock lock];
    BOOL registered = objc_getAssociatedObject(player, &QAVStateKey) != nil;
    [QAVLock unlock];
    if (registered) return;
    float base = hooks.getter(player, hooks.getSelector);
    [QAVLock lock];
    QAVRegisterLocked(player, hooks, base);
    [QAVLock unlock];
    QAVApplyLatest(player);
}

static void QAVReplace(Class cls, SEL selector, IMP replacement) {
    Method method = class_getInstanceMethod(cls, selector);
    if (!method) return;
    [QAVLock lock];
    BOOL inherited = [QAVInstalledIMPs containsObject:[NSValue valueWithPointer:method_getImplementation(method)]];
    [QAVInstalledIMPs addObject:[NSValue valueWithPointer:replacement]];
    [QAVLock unlock];
    if (inherited) return;
    if (!class_addMethod(cls, selector, replacement, method_getTypeEncoding(method))) {
        method_setImplementation(class_getInstanceMethod(cls, selector), replacement);
    }
}

static void QAVHookVoidTransport(Class cls, SEL selector, QAVVolumeHooks hooks) {
    Method method = class_getInstanceMethod(cls, selector);
    if (!method) return;
    void (*original)(id, SEL) = (void *)method_getImplementation(method);
    QAVReplace(cls, selector, imp_implementationWithBlock(^(id player) {
        if (!QAVTransportDepth) QAVPreparePlayback(player, hooks);
        QAVTransportDepth++;
        @try { original(player, selector); }
        @finally { QAVTransportDepth--; }
    }));
}

static void QAVHookObjectTransport(Class cls, SEL selector, QAVVolumeHooks hooks) {
    Method method = class_getInstanceMethod(cls, selector);
    if (!method) return;
    void (*original)(id, SEL, id) = (void *)method_getImplementation(method);
    QAVReplace(cls, selector, imp_implementationWithBlock(^(id player, id argument) {
        if (!QAVTransportDepth) QAVPreparePlayback(player, hooks);
        QAVTransportDepth++;
        @try { original(player, selector, argument); }
        @finally { QAVTransportDepth--; }
    }));
}

static void QAVMarkMixerActive(void) {
    [QAVLock lock];
    BOOL firstTime = !QAVMixerRegistered;
    QAVMixerRegistered = YES;
    if (firstTime) QAVGeneration++;
    [QAVLock unlock];
    // Nodes that were carrying the gain themselves must hand it back to the mixer.
    if (firstTime) QAVReapplyAll();
}

// Resolves the untouched accessors of a class, or reports that a hook of ours already answers them, so a subclass
// that merely inherits one can never be captured as if it were the native implementation and scale twice.
static BOOL QAVResolveNativeHooks(Class cls, SEL setVolumeSelector, SEL getVolumeSelector, QAVVolumeHooks *out) {
    Method setMethod = class_getInstanceMethod(cls, setVolumeSelector);
    Method getMethod = class_getInstanceMethod(cls, getVolumeSelector);
    if (!setMethod || !getMethod) return NO;
    IMP setter = method_getImplementation(setMethod);
    IMP getter = method_getImplementation(getMethod);
    [QAVLock lock];
    BOOL alreadyHooked = [QAVInstalledIMPs containsObject:[NSValue valueWithPointer:setter]] ||
                         [QAVInstalledIMPs containsObject:[NSValue valueWithPointer:getter]];
    [QAVLock unlock];
    if (alreadyHooked) return NO;
    *out = (QAVVolumeHooks){
        .setter = (void *)setter,
        .getter = (void *)getter,
        .setSelector = setVolumeSelector,
        .getSelector = getVolumeSelector
    };
    return YES;
}

static void QAVHookVolumeClass(Class cls, NSString *name, SEL setVolumeSelector, SEL getVolumeSelector) {
    if (!cls) return;
    BOOL isMixer = [name isEqualToString:@"AVAudioMixerNode"];
    QAVVolumeHooks hooks;
    if (!QAVResolveNativeHooks(cls, setVolumeSelector, getVolumeSelector, &hooks)) return;
    hooks.defersToMixer = [name isEqualToString:@"AVAudioPlayerNode"];
    if (isMixer) {
        QAVMixerHooks = hooks;
        QAVMixerHooksAvailable = YES;
    }
    if ([name isEqualToString:@"AVAudioPlayer"]) {
        QAVAudioPlayerFade = (void *)class_getMethodImplementation(cls, @selector(setVolume:fadeDuration:));
    }

    QAVReplace(cls, setVolumeSelector, imp_implementationWithBlock(^(id player, float volume) {
        if (QAVInternalVolumeWrite) {
            hooks.setter(player, setVolumeSelector, volume);
            return;
        }
        [QAVLock lock];
        QAVPlayerState *state = QAVRegisterLocked(player, hooks, volume);
        state.policy = (QAVVolumePolicy){ .requestedVolume = volume };
        state.revision++;
        QAVSetterHits[name] = @(QAVSetterHits[name].unsignedIntegerValue + 1);
        [QAVLock unlock];
        QAVApplyLatest(player);
        if (!isMixer) return;
        // Read the value back through the untouched accessor: it is the only proof that the scaled volume landed
        // on the object the engine actually mixes with, rather than on a class we merely thought was in charge.
        QAVInternalVolumeWrite++;
        float applied = hooks.getter(player, getVolumeSelector);
        QAVInternalVolumeWrite--;
        [QAVLock lock];
        QAVMixerWrites++;
        QAVMixerRequested = volume;
        QAVMixerApplied = applied;
        QAVMixerClassName = NSStringFromClass([player class]);
        [QAVLock unlock];
        QAVMarkMixerActive();
    }));

    QAVReplace(cls, getVolumeSelector, imp_implementationWithBlock(^float(id player) {
        if (QAVInternalVolumeWrite) return hooks.getter(player, getVolumeSelector);
        [QAVLock lock];
        QAVPlayerState *state = objc_getAssociatedObject(player, &QAVStateKey);
        QAVVolumePolicy policy = state.policy;
        [QAVLock unlock];
        return state ? QAVRequestedVolumeAt(policy, NSProcessInfo.processInfo.systemUptime) : hooks.getter(player, getVolumeSelector);
    }));

    if (isMixer) return;

    if ([name isEqualToString:@"AVAudioPlayer"]) {
        SEL play = @selector(play);
        BOOL (*originalPlay)(id, SEL) = (void *)class_getMethodImplementation(cls, play);
        QAVReplace(cls, play, imp_implementationWithBlock(^BOOL(id player) {
            if (!QAVTransportDepth) QAVPreparePlayback(player, hooks);
            QAVTransportDepth++;
            @try { return originalPlay(player, play); }
            @finally { QAVTransportDepth--; }
        }));
        SEL timedPlay = @selector(playAtTime:);
        BOOL (*originalTimedPlay)(id, SEL, NSTimeInterval) = (void *)class_getMethodImplementation(cls, timedPlay);
        QAVReplace(cls, timedPlay, imp_implementationWithBlock(^BOOL(id player, NSTimeInterval time) {
            if (!QAVTransportDepth) QAVPreparePlayback(player, hooks);
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
            QAVPreparePlayback(player, hooks);
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
            if (!QAVTransportDepth) QAVPreparePlayback(player, hooks);
            QAVTransportDepth++;
            @try { original(player, enqueue, buffer); }
            @finally { QAVTransportDepth--; }
        }));
    } else {
        QAVHookVoidTransport(cls, @selector(play), hooks);
        if ([name isEqualToString:@"AVAudioPlayerNode"]) {
            QAVHookObjectTransport(cls, @selector(playAtTime:), hooks);
        } else {
            SEL timedRate = @selector(setRate:time:atHostTime:);
            Method timedMethod = class_getInstanceMethod(cls, timedRate);
            if (timedMethod) {
                void (*originalTimedRate)(id, SEL, float, CMTime, CMTime) = (void *)method_getImplementation(timedMethod);
                QAVReplace(cls, timedRate, imp_implementationWithBlock(^(id player, float rate, CMTime time, CMTime hostTime) {
                    if (rate != 0.0f && !QAVTransportDepth) QAVPreparePlayback(player, hooks);
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
                    if (rate != 0.0f && !QAVTransportDepth) QAVPreparePlayback(player, hooks);
                    QAVTransportDepth++;
                    @try { original(player, selector, rate); }
                    @finally { QAVTransportDepth--; }
                }));
            }
        }
    }
}

static void QAVHookClass(NSString *name, SEL setVolumeSelector, SEL getVolumeSelector) {
    QAVHookVolumeClass(NSClassFromString(name), name, setVolumeSelector, getVolumeSelector);
}

static void QAVAdoptEngineMixer(id engine) {
    AVAudioNode *mixer = [(AVAudioEngine *)engine mainMixerNode];
    if (!mixer) return;
    Class concrete = object_getClass(mixer);
    Class declared = NSClassFromString(@"AVAudioMixerNode");
    // A private mixer subclass can override the accessors, so the hook belongs on the class that answers the call.
    // An isa-swizzled KVO class is left alone: replacing its setter would swallow the observer's notifications.
    if (concrete && declared && concrete != declared && concrete == mixer.class) {
        QAVHookVolumeClass(concrete, @"AVAudioMixerNode", @selector(setOutputVolume:), @selector(outputVolume));
    }
    [QAVLock lock];
    QAVMixerClassName = NSStringFromClass(mixer.class);
    [QAVLock unlock];
    QAVPreparePlayback(mixer, QAVMixerHooks);
    QAVMarkMixerActive();
}

// An engine that never writes outputVolume still mixes through mainMixerNode, so claim it as the boundary at start.
static void QAVHookEngine(void) {
    if (!QAVMixerHooksAvailable) return;
    Class cls = NSClassFromString(@"AVAudioEngine");
    if (!cls) return;
    SEL start = @selector(startAndReturnError:);
    Method startMethod = class_getInstanceMethod(cls, start);
    if (startMethod) {
        BOOL (*original)(id, SEL, NSError **) = (void *)method_getImplementation(startMethod);
        QAVReplace(cls, start, imp_implementationWithBlock(^BOOL(id engine, NSError **error) {
            if (!QAVTransportDepth) QAVAdoptEngineMixer(engine);
            QAVTransportDepth++;
            @try { return original(engine, start, error); }
            @finally { QAVTransportDepth--; }
        }));
    }
    SEL prepare = @selector(prepare);
    Method prepareMethod = class_getInstanceMethod(cls, prepare);
    if (prepareMethod) {
        void (*original)(id, SEL) = (void *)method_getImplementation(prepareMethod);
        QAVReplace(cls, prepare, imp_implementationWithBlock(^(id engine) {
            if (!QAVTransportDepth) QAVAdoptEngineMixer(engine);
            QAVTransportDepth++;
            @try { original(engine, prepare); }
            @finally { QAVTransportDepth--; }
        }));
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
    title.text = @"App 独立音量 · 0.4.2";
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
    help.text = @"三指双击打开此面板。设置仅保存在当前 App 内；100% 保留播放器原音量，0% 静音。覆盖 AVPlayer、AVAudioPlayer、AVAudioPlayerNode、AVSampleBufferAudioRenderer、AudioQueue、AVAudioEngine 混音器，以及 App 自己启动的 RemoteIO 渲染缓冲。不改变系统音量或后台播放策略。";
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
- (NSString *)mixerSummaryLockedRead {
    [QAVLock lock];
    BOOL active = QAVMixerRegistered;
    NSUInteger writes = QAVMixerWrites;
    float requested = QAVMixerRequested;
    float applied = QAVMixerApplied;
    NSString *className = QAVMixerClassName.copy;
    [QAVLock unlock];
    if (!active) return @"未接管";
    return [NSString stringWithFormat:@"%@：已接管，写入 %lu 次，最近请求 %.3f → 落地 %.3f", className ?: @"未知类", (unsigned long)writes, requested, applied];
}
- (NSString *)setterHitSummary {
    [QAVLock lock];
    NSArray<NSString *> *names = @[ @"AVPlayer", @"AVAudioPlayer", @"AVAudioPlayerNode", @"AVSampleBufferAudioRenderer", @"AVAudioMixerNode" ];
    NSMutableArray<NSString *> *parts = [NSMutableArray new];
    for (NSString *name in names) [parts addObject:[NSString stringWithFormat:@"%@:%@", name, QAVSetterHits[name] ?: @"0"]];
    [QAVLock unlock];
    return [parts componentsJoinedByString:@" "];
}
// One line per candidate the speaker stream could be, with how many frames each actually scaled: when 缓冲不符
// climbs, the observed render geometry printed next to these candidates shows which assumption is wrong.
- (NSString *)candidateSummary:(QAVAudioUnitDiagnostics)units {
    if (!units.candidateCount) return @"无（未认出喇叭格式）";
    NSMutableArray<NSString *> *parts = [NSMutableArray new];
    for (uint32_t index = 0; index < units.candidateCount && index < QAV_AUDIO_UNIT_CANDIDATES; index++) {
        [parts addObject:[NSString stringWithFormat:@"c%u %uB/帧 %u bit %@，缩放 %llu 帧",
            index, units.candidateBytesPerFrame[index], units.candidateBits[index],
            units.candidateIsFloat[index] ? @"浮点" : @"整型",
            (unsigned long long)units.framesPerCandidate[index]]];
    }
    return [parts componentsJoinedByString:@"；"];
}
// The fourcc as the SDK writes it, so an unexpected component subtype or a rejected mFormatID can be read off the
// panel instead of guessed from a decimal code.
static NSString *QAVFourCC(uint32_t value) {
    if (!value) return @"-";
    char raw[4] = { (char)(value >> 24), (char)(value >> 16), (char)(value >> 8), (char)value };
    NSMutableString *text = [NSMutableString stringWithString:@"'"];
    for (int index = 0; index < 4; index++) {
        char symbol = raw[index];
        [text appendString:(symbol >= 0x20 && symbol <= 0x7e) ? [NSString stringWithFormat:@"%c", symbol] : @"?"];
    }
    [text appendString:@"'"];
    return text;
}
// Kept whenever a readable stream description was still refused: the exact bits/channels/bytes-per-frame tell apart
// a 24-bit packed layout, a padded one, and a probe that simply has no format yet.
- (NSString *)rejectedFormatSummary:(QAVAudioUnitDiagnostics)units {
    if (!units.rejectedFormatID) return @"无（两份描述都探不到，或已被候选覆盖）";
    return [NSString stringWithFormat:@"%@ %.0fHz，%u bit，%u 声道，%u B/帧，flags 0x%X",
        QAVFourCC(units.rejectedFormatID), units.rejectedSampleRate, units.rejectedBits, units.rejectedChannels,
        units.rejectedBytesPerFrame, units.rejectedFlags];
}
- (void)refreshDiagnostics {
    [QAVLock lock];
    NSUInteger count = QAVPlayers.allObjects.count;
    [QAVLock unlock];
    QAVAudioUnitRefresh();
    QAVAudioQueueDiagnostics queues = QAVAudioQueueGetDiagnostics();
    QAVAudioUnitDiagnostics units = QAVAudioUnitGetDiagnostics();
    char bundle[PATH_MAX] = "", image[PATH_MAX] = "";
    QAVAudioQueueCopyPaths(bundle, sizeof(bundle), image, sizeof(image));
    self.diagnostics.text = [NSString stringWithFormat:
        @"AVFoundation：%lu 个（音量写入 %@）\nAVAudioEngine 混音器%@\nAudioQueue：%llu 个（累计 %llu），音量应用 %llu 次，队列错误 %d\nAudioUnit：观察到 %llu 个，卸载 %llu 个，App 内新建 %llu 个（最近类型 %@/%@）\n喇叭 bus 缩放 %llu 帧，其它 bus 跳过 %llu 帧，缓冲不符 %llu 次\n各 bus 帧数：b0=%llu b1=%llu b2=%llu b3+=%llu\n候选格式：%@\n实测渲染：%u 帧 / %u 字节 / %u 个缓冲\n喇叭格式 status：输入 %d，输出 %d，%u 声道，%u B/帧\n未识别格式：%@\n麦克风格式：status %d，%u bit\nEnableIO：输出 %d，输入 %d，格式写入 %llu 次，不认识格式 %llu，挂载错误 %d\n已扫描应用模块：%llu / 已见模块：%llu，拦截状态 %d（errno %d）\n应用目录：%@\n首个可见模块：%@\n音量在下一次渲染（或下一次音量写入）时生效。",
        (unsigned long)count,
        [self setterHitSummary],
        [self mixerSummaryLockedRead],
        (unsigned long long)queues.activeQueues,
        (unsigned long long)queues.createdQueues,
        (unsigned long long)queues.appliedChanges,
        queues.lastError,
        (unsigned long long)units.attachedUnits,
        (unsigned long long)units.detachedUnits,
        (unsigned long long)units.createdInstances,
        QAVFourCC(units.createdType),
        QAVFourCC(units.createdSubType),
        (unsigned long long)units.scaledFrames,
        (unsigned long long)units.skippedFrames,
        (unsigned long long)units.geometryMismatches,
        (unsigned long long)units.framesPerBus[0],
        (unsigned long long)units.framesPerBus[1],
        (unsigned long long)units.framesPerBus[2],
        (unsigned long long)units.framesPerBus[3],
        [self candidateSummary:units],
        units.renderFrames,
        units.renderBytes,
        units.renderBuffers,
        units.speakerFormatStatus,
        units.hardwareFormatStatus,
        units.speakerChannels,
        units.speakerBytesPerFrame,
        [self rejectedFormatSummary:units],
        units.micFormatStatus,
        units.micBitsPerChannel,
        units.speakerIO,
        units.micIO,
        (unsigned long long)units.formatEvents,
        (unsigned long long)units.formatFailures,
        units.lastError,
        (unsigned long long)queues.scannedImages,
        (unsigned long long)queues.seenImages,
        queues.installStatus,
        queues.installErrno,
        @(bundle),
        @(image)];
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
        QAVInstalledIMPs = [NSMutableSet set];
        QAVSetterHits = [NSMutableDictionary dictionary];
        QAVPreferenceKey = [@"io.coyote.AppVolume.settings." stringByAppendingString:bundle.bundleIdentifier ?: @"main"];
        id saved = [[NSUserDefaults standardUserDefaults] objectForKey:QAVPreferenceKey];
        if ([saved isKindOfClass:NSDictionary.class]) {
            id gain = saved[@"gain"];
            id enabled = saved[@"enabled"];
            if ([gain isKindOfClass:NSNumber.class]) QAVGain = QAVNormalizeGain([gain floatValue]);
            if ([enabled isKindOfClass:NSNumber.class]) QAVEnabled = [enabled boolValue];
        }
        SEL volume = @selector(volume);
        QAVHookClass(@"AVPlayer", @selector(setVolume:), volume);
        QAVHookClass(@"AVAudioPlayer", @selector(setVolume:), volume);
        QAVHookClass(@"AVAudioPlayerNode", @selector(setVolume:), volume);
        QAVHookClass(@"AVSampleBufferAudioRenderer", @selector(setVolume:), volume);
        QAVHookClass(@"AVAudioMixerNode", @selector(setOutputVolume:), @selector(outputVolume));
        QAVHookEngine();
        QAVNativeAudioInstall(bundle.bundlePath.fileSystemRepresentation, QAVGain, QAVEnabled);
        dispatch_async(dispatch_get_main_queue(), ^{
            static QAVUIManager *manager;
            manager = [QAVUIManager new];
            [manager attachWindows];
        });
    }
}
