// Eternal Darkness.app's own program. The app holds no game: on the first
// start it asks for the player's disc image, translates it (edprep, DolRecomp)
// and builds the game from it with the bundled Zig, into
// ~/Library/Application Support/Eternal Darkness, then starts the game there.
// Later starts go straight to the game. Hold Option while starting to choose
// another disc. Built by tools/mac/make-mac-app.sh.
#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <fcntl.h>
#include <mach-o/loader.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

static NSString* g_resources;   // Contents/Resources
static NSString* g_frameworks;  // Contents/Frameworks
static NSString* g_data;        // ~/Library/Application Support/Eternal Darkness
static NSString* g_version;     // Contents/Resources/VERSION

static NSString* data_path(NSString* name) { return [g_data stringByAppendingPathComponent:name]; }
static NSString* res_path(NSString* name) { return [g_resources stringByAppendingPathComponent:name]; }

static NSString* read_text(NSString* path) {
    NSString* s = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    return s != nil ? [s stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] : @"";
}

static void write_text(NSString* path, NSString* text) {
    [text writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

static int cpu_count(void) {
    int n = 0;
    size_t size = sizeof n;
    if (sysctlbyname("hw.perflevel0.logicalcpu", &n, &size, NULL, 0) != 0 || n <= 0)
        n = (int)NSProcessInfo.processInfo.activeProcessorCount;
    return n > 0 ? n : 4;
}

// The disc this build was made from and when, with the app's version: a
// change to any of them builds again.
static NSString* stamp_for(NSString* disc) {
    struct stat st;
    if (stat(disc.fileSystemRepresentation, &st) != 0)
        return nil;
    return [NSString stringWithFormat:@"%@\n%lld %ld\n%@", disc, (long long)st.st_size,
                                      (long)st.st_mtimespec.tv_sec, g_version];
}

// A linked game: a 64-bit Mach-O of some size (a failed link can leave an
// empty file behind).
static BOOL is_game_binary(NSString* path) {
    struct stat st;
    if (stat(path.fileSystemRepresentation, &st) != 0 || st.st_size < (1 << 20))
        return NO;
    const int fd = open(path.fileSystemRepresentation, O_RDONLY);
    uint32_t magic = 0;
    const BOOL ok = fd >= 0 && read(fd, &magic, sizeof magic) == sizeof magic && magic == MH_MAGIC_64;
    if (fd >= 0)
        close(fd);
    return ok;
}

static BOOL game_ready(NSString* disc) {
    NSString* stamp = stamp_for(disc);
    return stamp != nil && is_game_binary(data_path(@"game/EternalDarkness")) &&
           [read_text(data_path(@"game/build.stamp")) isEqualToString:stamp];
}

// Copy the app's menus, fonts and icons beside the game when the app changes.
static void sync_res(void) {
    NSFileManager* fm = NSFileManager.defaultManager;
    if ([read_text(data_path(@"res/VERSION")) isEqualToString:g_version])
        return;
    [fm removeItemAtPath:data_path(@"res") error:nil];
    [fm copyItemAtPath:res_path(@"res") toPath:data_path(@"res") error:nil];
    write_text(data_path(@"res/VERSION"), g_version);
}

// Start the game in place of this program (its window takes over). Returns
// only if it could not: the build is then forgotten, so the next try builds.
static void start_game(NSString* disc) {
    sync_res();
    NSFileManager* fm = NSFileManager.defaultManager;
    [fm createDirectoryAtPath:data_path(@"logs") withIntermediateDirectories:YES attributes:nil error:nil];
    [fm removeItemAtPath:data_path(@"logs/game-prev.log") error:nil];
    [fm moveItemAtPath:data_path(@"logs/game.log") toPath:data_path(@"logs/game-prev.log") error:nil];
    const int log = open(data_path(@"logs/game.log").fileSystemRepresentation,
                         O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log >= 0) {
        dup2(log, STDOUT_FILENO);
        dup2(log, STDERR_FILENO);
        close(log);
    }
    chdir(g_data.fileSystemRepresentation);
    const char* exe = data_path(@"game/EternalDarkness").fileSystemRepresentation;
    char* const argv[] = {(char*)exe, "game/main.dol", "--iso", (char*)disc.fileSystemRepresentation,
                          "--max-blocks", "1000000000000000", NULL};
    execv(exe, argv);
    fprintf(stderr, "could not start %s: %s\n", exe, strerror(errno));
    [fm removeItemAtPath:data_path(@"game/build.stamp") error:nil];
}

// --- building ---------------------------------------------------------------

// Run a tool, its output appended to logs/build.log; on_line gets its stdout
// lines when given. Returns the exit status (-1 if it could not start).
static int run_tool(NSArray<NSString*>* args, NSDictionary<NSString*, NSString*>* env,
                    void (^on_line)(NSString*)) {
    NSTask* task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:args[0]];
    task.arguments = [args subarrayWithRange:NSMakeRange(1, args.count - 1)];
    if (env != nil)
        task.environment = env;
    NSFileHandle* log = [NSFileHandle fileHandleForWritingAtPath:data_path(@"logs/build.log")];
    [log seekToEndOfFile];
    NSPipe* pipe = [NSPipe pipe];
    task.standardOutput = on_line != nil ? pipe : log;
    task.standardError = log;
    if (![task launchAndReturnError:nil])
        return -1;
    if (on_line != nil) {
        NSMutableData* pending = [NSMutableData data];
        NSData* chunk;
        while ((chunk = [pipe.fileHandleForReading availableData]).length != 0) {
            [log writeData:chunk];
            [pending appendData:chunk];
            for (;;) {
                const char* bytes = pending.bytes;
                const void* nl = memchr(bytes, '\n', pending.length);
                if (nl == NULL)
                    break;
                const NSUInteger n = (NSUInteger)((const char*)nl - bytes);
                on_line([[NSString alloc] initWithBytes:bytes length:n encoding:NSUTF8StringEncoding]);
                [pending replaceBytesInRange:NSMakeRange(0, n + 1) withBytes:NULL length:0];
            }
        }
    }
    [task waitUntilExit];
    return task.terminationStatus;
}

@interface Builder : NSObject
@property(copy) void (^progress)(double fraction, NSString* status);
- (NSString*)buildFromDisc:(NSString*)disc;  // nil when done, else the error
@end

@implementation Builder

- (NSString*)buildFromDisc:(NSString*)disc {
    NSFileManager* fm = NSFileManager.defaultManager;
    NSString* build = data_path(@"build");
    NSString* generated = [build stringByAppendingPathComponent:@"generated"];
    NSString* obj = [build stringByAppendingPathComponent:@"obj"];
    [fm removeItemAtPath:build error:nil];
    [fm createDirectoryAtPath:obj withIntermediateDirectories:YES attributes:nil error:nil];
    [fm createDirectoryAtPath:data_path(@"logs") withIntermediateDirectories:YES attributes:nil error:nil];
    [fm createDirectoryAtPath:data_path(@"game") withIntermediateDirectories:YES attributes:nil error:nil];
    write_text(data_path(@"logs/build.log"), @"");
    const int jobs = cpu_count();

    // 1. Translate the disc.
    self.progress(0.01, @"Reading your disc…");
    __block NSString* step = nil;
    const int prep = run_tool(@[ res_path(@"tools/edprep"), disc, @"--out", generated,
                                 @"--dolrecomp", res_path(@"tools/dolrecomp"),
                                 @"--symbols", res_path(@"tools/symbols.txt"),
                                 @"--jobs", @(jobs).stringValue ],
                              nil, ^(NSString* line) {
                                  if ([line hasPrefix:@"step 2/"])
                                      step = @"Translating the game's code…";
                                  else if ([line hasPrefix:@"step 4/"])
                                      step = @"Reading the game's symbols…";
                                  if (step != nil)
                                      self.progress(0.04, step);
                              });
    if (prep != 0)
        return @"This disc image could not be read as Eternal Darkness (USA GEDE01 or "
               @"European GEDP01).";
    NSString* gameId = read_text([generated stringByAppendingPathComponent:@"game_id.txt"]);

    // 2. Compile: the chunks hot in play (profiled, tools/deck/hot-*.txt) at
    //    -O1, the rest at -O0, the host's disc-dependent files at -O2.
    NSSet* hot = [NSSet setWithArray:[read_text(res_path([NSString stringWithFormat:@"tools/hot-%@.txt", gameId]))
                                         componentsSeparatedByString:@"\n"]];
    NSMutableArray<NSArray*>* units = [NSMutableArray array];
    NSString* chunks = [generated stringByAppendingPathComponent:@"chunks"];
    for (NSString* name in [[fm contentsOfDirectoryAtPath:chunks error:nil]
             sortedArrayUsingSelector:@selector(compare:)]) {
        if (![name hasSuffix:@".c"])
            continue;
        NSString* base = name.stringByDeletingPathExtension;
        NSString* addr = [base componentsSeparatedByString:@"_"].lastObject;
        [units addObject:@[ [chunks stringByAppendingPathComponent:name], [hot containsObject:addr] ? @"1" : @"0" ]];
    }
    [units addObject:@[ [generated stringByAppendingPathComponent:@"generated.c"], @"1" ]];
    for (NSString* f in @[ @"main.c", @"hle.c", @"audio.c", @"sdk_map.c", @"quickstate.c" ])
        [units addObject:@[ res_path([@"sdk/runtime/host" stringByAppendingPathComponent:f]), @"2" ]];
    // Hot first: they take longest.
    [units sortUsingComparator:^NSComparisonResult(NSArray* a, NSArray* b) {
        return [b[1] compare:a[1]];
    }];

    NSMutableDictionary* env = [NSProcessInfo.processInfo.environment mutableCopy];
    env[@"ZIG_GLOBAL_CACHE_DIR"] = data_path(@"zig-cache");
    env[@"ZIG_LOCAL_CACHE_DIR"] = data_path(@"zig-cache");
    env[@"DEVELOPER_DIR"] = @"/nonexistent";  // the bundled macOS headers, not Xcode's
    env[@"PATH"] = @"/usr/bin:/bin";
    [env removeObjectForKey:@"SDKROOT"];
    NSString* zig = res_path(@"zig/zig");
    NSMutableArray<NSString*>* flags = [NSMutableArray arrayWithArray:@[
        @"cc", @"-target", @"aarch64-macos.13.0", @"-std=gnu11", @"-g0", @"-w", @"-fno-sanitize=all",
        [NSString stringWithFormat:@"-DED_GAME_ID=\"%@\"", gameId],
        [@"-I" stringByAppendingString:res_path(@"sdk/runtime")],
        [@"-I" stringByAppendingString:generated],
        [@"-I" stringByAppendingString:res_path(@"sdk/include")]
    ]];
    for (NSString* d in [read_text(res_path(@"sdk/cflags.txt")) componentsSeparatedByString:@" "])
        if (d.length != 0)
            [flags addObject:d];

    self.progress(0.05, @"Building the game from your disc (only the first time)…");
    const char* log_path = data_path(@"logs/build.log").fileSystemRepresentation;
    NSUInteger next = 0, done = 0, running = 0;
    BOOL failed = NO;
    while (done < units.count) {
        while (!failed && running < (NSUInteger)jobs && next < units.count) {
            NSArray* unit = units[next++];
            NSString* src = unit[0];
            NSString* out = [obj stringByAppendingPathComponent:
                                     [src.lastPathComponent stringByAppendingString:@".o"]];
            NSMutableArray<NSString*>* args = [NSMutableArray arrayWithObject:zig];
            [args addObjectsFromArray:flags];
            [args addObjectsFromArray:@[ [@"-O" stringByAppendingString:unit[1]], @"-c", src, @"-o", out ]];
            const size_t n = args.count;
            char** argv = calloc(n + 1, sizeof(char*));
            for (size_t i = 0; i < n; i++)
                argv[i] = strdup(args[i].fileSystemRepresentation);
            NSMutableArray<NSString*>* envs = [NSMutableArray array];
            for (NSString* k in env)
                [envs addObject:[NSString stringWithFormat:@"%@=%@", k, env[k]]];
            char** envp = calloc(envs.count + 1, sizeof(char*));
            for (NSUInteger i = 0; i < envs.count; i++)
                envp[i] = strdup(envs[i].UTF8String);
            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log_path, O_WRONLY | O_APPEND, 0);
            posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, log_path, O_WRONLY | O_APPEND, 0);
            pid_t pid;
            const int err = posix_spawn(&pid, argv[0], &actions, NULL, argv, envp);
            posix_spawn_file_actions_destroy(&actions);
            for (size_t i = 0; i < n; i++)
                free(argv[i]);
            free(argv);
            for (NSUInteger i = 0; i < envs.count; i++)
                free(envp[i]);
            free(envp);
            if (err != 0) {
                failed = YES;
                done++;
            } else {
                running++;
            }
        }
        if (running == 0)
            break;
        int status = 0;
        if (waitpid(-1, &status, 0) > 0) {
            running--;
            done++;
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                failed = YES;
            self.progress(0.05 + 0.9 * (double)done / units.count,
                          [NSString stringWithFormat:@"Building the game from your disc (only the "
                                                     @"first time)… %lu of %lu",
                                                     (unsigned long)done, (unsigned long)units.count]);
        }
    }
    if (failed)
        return @"Building the game failed.";
    for (NSArray* unit in units) {
        NSString* out = [obj stringByAppendingPathComponent:[((NSString*)unit[0]).lastPathComponent
                                                                stringByAppendingString:@".o"]];
        NSDictionary* attrs = [fm attributesOfItemAtPath:out error:nil];
        if (attrs == nil || attrs.fileSize == 0)
            return @"Building the game failed.";
    }

    // 3. Link against the host library, beside which the game runs.
    self.progress(0.96, @"Linking…");
    NSMutableArray<NSString*>* link = [NSMutableArray arrayWithArray:@[
        zig, @"cc", @"-target", @"aarch64-macos.13.0", @"-o", data_path(@"game/EternalDarkness.new")
    ]];
    for (NSString* name in [fm contentsOfDirectoryAtPath:obj error:nil])
        [link addObject:[obj stringByAppendingPathComponent:name]];
    [link addObjectsFromArray:@[ [g_frameworks stringByAppendingPathComponent:@"libedhost.dylib"],
                                 @"-Wl,-rpath,@executable_path" ]];
    if (run_tool(link, env, nil) != 0 || !is_game_binary(data_path(@"game/EternalDarkness.new")))
        return @"Linking the game failed.";
    [fm removeItemAtPath:data_path(@"game/libedhost.dylib") error:nil];
    [fm copyItemAtPath:[g_frameworks stringByAppendingPathComponent:@"libedhost.dylib"]
                toPath:data_path(@"game/libedhost.dylib") error:nil];
    [fm removeItemAtPath:data_path(@"game/main.dol") error:nil];
    [fm copyItemAtPath:[generated stringByAppendingPathComponent:@"main.dol"]
                toPath:data_path(@"game/main.dol") error:nil];
    [fm removeItemAtPath:data_path(@"game/EternalDarkness") error:nil];
    [fm moveItemAtPath:data_path(@"game/EternalDarkness.new") toPath:data_path(@"game/EternalDarkness") error:nil];
    write_text(data_path(@"game/build.stamp"), stamp_for(disc));
    // Neither the translated code nor Zig's cache (100 MB) is needed again.
    [fm removeItemAtPath:build error:nil];
    [fm removeItemAtPath:data_path(@"zig-cache") error:nil];
    self.progress(1.0, @"Ready.");
    return nil;
}

@end

// --- the window ---------------------------------------------------------------

@interface DropView : NSView
@property(copy) void (^onDisc)(NSString* path);
@end

@implementation DropView
- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame]))
        [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    return self;
}
- (NSString*)discIn:(id<NSDraggingInfo>)info {
    NSArray<NSURL*>* urls = [info.draggingPasteboard readObjectsForClasses:@[ NSURL.class ]
                                                                   options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    for (NSURL* url in urls) {
        NSString* ext = url.pathExtension.lowercaseString;
        if ([ext isEqualToString:@"iso"] || [ext isEqualToString:@"gcm"] || [ext isEqualToString:@"ciso"])
            return url.path;
    }
    return nil;
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info {
    return self.onDisc != nil && [self discIn:info] != nil ? NSDragOperationCopy : NSDragOperationNone;
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
    NSString* disc = [self discIn:info];
    if (disc == nil || self.onDisc == nil)
        return NO;
    self.onDisc(disc);
    return YES;
}
@end

@interface AppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation AppDelegate {
    BOOL _building;
    NSWindow* _window;
    DropView* _view;
    NSTextField* _text;
    NSTextField* _status;
    NSProgressIndicator* _bar;
    NSButton* _choose;
}

- (NSTextField*)label:(NSString*)s frame:(NSRect)r font:(NSFont*)f {
    NSTextField* t = [NSTextField wrappingLabelWithString:s];
    t.frame = r;
    t.font = f;
    t.alignment = NSTextAlignmentCenter;
    [_view addSubview:t];
    return t;
}

- (void)makeWindow {
    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 560, 320)
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _window.title = @"Eternal Darkness";
    _view = [[DropView alloc] initWithFrame:NSMakeRect(0, 0, 560, 320)];
    _window.contentView = _view;
    NSImageView* icon = [NSImageView imageViewWithImage:NSApp.applicationIconImage];
    icon.frame = NSMakeRect(248, 236, 64, 64);
    [_view addSubview:icon];
    [self label:@"Eternal Darkness: Sanity's Requiem" frame:NSMakeRect(30, 200, 500, 24)
           font:[NSFont boldSystemFontOfSize:17]];
    _text = [self label:@"This app holds no game. Drop your own disc image here (an .iso or .gcm "
                        @"of the USA or European GameCube disc), or choose it.\n\nThe first start "
                        @"translates it and builds the game from it, which takes a minute or two. "
                        @"After that the game starts straight away."
                  frame:NSMakeRect(40, 100, 480, 90)
                   font:[NSFont systemFontOfSize:13]];
    _choose = [NSButton buttonWithTitle:@"Choose Disc Image…" target:self action:@selector(choose:)];
    _choose.frame = NSMakeRect(190, 50, 180, 32);
    [_view addSubview:_choose];
    _bar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(60, 70, 440, 20)];
    _bar.indeterminate = NO;
    _bar.minValue = 0;
    _bar.maxValue = 1;
    _bar.hidden = YES;
    [_view addSubview:_bar];
    _status = [self label:@"" frame:NSMakeRect(40, 30, 480, 34) font:[NSFont systemFontOfSize:12]];
    __weak AppDelegate* weak = self;
    _view.onDisc = ^(NSString* disc) { [weak build:disc]; };
    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)choose:(id)sender {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.message = @"Choose your Eternal Darkness disc image";
    NSMutableArray* types = [NSMutableArray array];
    for (NSString* ext in @[ @"iso", @"gcm", @"ciso" ]) {
        UTType* t = [UTType typeWithFilenameExtension:ext];
        if (t != nil)
            [types addObject:t];
    }
    panel.allowedContentTypes = types;
    if ([panel runModal] == NSModalResponseOK)
        [self build:panel.URL.path];
}

- (void)build:(NSString*)disc {
    if (_building)
        return;
    _building = YES;
    _view.onDisc = nil;
    _choose.hidden = YES;
    _bar.hidden = NO;
    _bar.doubleValue = 0;
    _text.stringValue = [NSString stringWithFormat:@"Preparing your game from\n%@", disc.lastPathComponent];
    Builder* builder = [Builder new];
    NSProgressIndicator* bar = _bar;
    NSTextField* status = _status;
    builder.progress = ^(double fraction, NSString* text) {
        dispatch_async(dispatch_get_main_queue(), ^{
            bar.doubleValue = fraction;
            status.stringValue = text;
        });
    };
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString* error = [builder buildFromDisc:disc];
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString* message = error;
            if (error == nil) {
                write_text(data_path(@"disc.txt"), disc);
                start_game(disc);
                message = @"The game was built but could not start.";
            }
            self->_building = NO;
            NSAlert* alert = [NSAlert new];
            alert.messageText = message;
            alert.informativeText = [NSString stringWithFormat:@"The details are in %@.", data_path(@"logs/build.log")];
            [alert runModal];
            self->_bar.hidden = YES;
            self->_choose.hidden = NO;
            self->_status.stringValue = @"";
            __weak AppDelegate* weak = self;
            self->_view.onDisc = ^(NSString* d) { [weak build:d]; };
        });
    });
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
    [self makeWindow];
    // A disc chosen before, still where it was, and the game built from it.
    NSString* disc = read_text(data_path(@"disc.txt"));
    const BOOL pick = (NSEvent.modifierFlags & NSEventModifierFlagOption) != 0;
    if (!pick && disc.length != 0 && [NSFileManager.defaultManager fileExistsAtPath:disc]) {
        if (game_ready(disc))
            start_game(disc);
        [self build:disc];  // the app or the disc changed, or it would not start: build again
    }
}

// A disc image opened with the app (Open With, or dropped on its icon).
- (void)application:(NSApplication*)app openURLs:(NSArray<NSURL*>*)urls {
    for (NSURL* url in urls) {
        NSString* ext = url.pathExtension.lowercaseString;
        if (url.isFileURL && ([ext isEqualToString:@"iso"] || [ext isEqualToString:@"gcm"] ||
                              [ext isEqualToString:@"ciso"])) {
            if (_window == nil)
                [self makeWindow];
            [self build:url.path];
            return;
        }
    }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {
    return YES;
}

@end

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        NSBundle* bundle = NSBundle.mainBundle;
        g_resources = bundle.resourcePath;
        g_frameworks = bundle.privateFrameworksPath;
        g_version = read_text(res_path(@"VERSION"));
        NSURL* support = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory
                                                              inDomains:NSUserDomainMask].firstObject;
        g_data = [support.path stringByAppendingPathComponent:@"Eternal Darkness"];
        // ED_APP_DATA=<folder>: somewhere else (tests).
        NSString* data_env = NSProcessInfo.processInfo.environment[@"ED_APP_DATA"];
        if (data_env.length != 0)
            g_data = data_env;
        [NSFileManager.defaultManager createDirectoryAtPath:g_data withIntermediateDirectories:YES
                                                 attributes:nil error:nil];
        // One at a time: two builds in this folder delete each other's files.
        // The lock stays open into the game (exec keeps it), so a second start
        // while it runs stops here too.
        const int lock = open(data_path(@".lock").fileSystemRepresentation, O_RDWR | O_CREAT, 0644);
        if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) != 0) {
            [NSApplication sharedApplication];
            NSAlert* alert = [NSAlert new];
            alert.messageText = @"Eternal Darkness is already open.";
            alert.informativeText = @"It is preparing your game or playing it in another window.";
            [alert runModal];
            return 0;
        }
        // Straight to the game when it is built (no window at all).
        NSString* disc = read_text(data_path(@"disc.txt"));
        if (disc.length != 0 && game_ready(disc) &&
            (NSEvent.modifierFlags & NSEventModifierFlagOption) == 0)
            start_game(disc);
        NSApplication* app = NSApplication.sharedApplication;
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        AppDelegate* delegate = [AppDelegate new];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
