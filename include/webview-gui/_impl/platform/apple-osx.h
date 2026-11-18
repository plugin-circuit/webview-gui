#pragma once

#include "../../helpers.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CGGeometry.h>
#include <objc/runtime.h>
#include <objc/message.h>

#include <cstring>

namespace webview_gui {

// Objective-C helpers - we use `objc_msgSend` for "simple" return values (not structs, not floating-point)
namespace _objc {
	template<typename Return=id, class... Args>
	Return callSimple(id obj, const char *method, Args... args) {
		// We have to cast `objc_msgSend` to a version with the arguments/return types
		auto fn = (Return(*)(id, SEL, Args...))(objc_msgSend);
		auto selector = sel_registerName(method);
		if (!class_respondsToSelector(object_getClass(obj), selector)) return Return();
		return fn(obj, selector, args...);
	}

	template<typename Return=id, class... Args>
	Return callSimple(const char *className, const char *method, Args... args) {
		return callSimple<Return>((id)objc_getClass(className), method, args...);
	}

	template<class... Args>
	void callVoid(Args... args) {
		callSimple<void>(args...);
	}

	id nsNumber(bool b) {
		return callSimple("NSNumber", "numberWithBool:", b);
	}

	id nsString(const char *utf8) {
		return callSimple("NSString", "stringWithUTF8String:", utf8);
	}
	
	bool instanceOf(id obj, const char *className) {
		return callSimple<bool>(obj, "isKindOfClass:", (id)objc_getClass(className));
	}
	
	// Calls an Objective-C block (a completion or decision handler) with these arguments: the block ABI's layout,
	// since this file is C++ talking to the runtime.
	template<class... Args>
	inline void callBlock(void *block, Args... args) {
		struct BlockLayout { void *isa; int flags; int reserved; void (*invoke)(void *, Args...); };
		auto *b = (BlockLayout *)block;
		if (b && b->invoke) b->invoke(b, args...);
	}

	// http(s) and mailto links open in the user's browser / mail app, never inside the plugin's GUI
	inline bool openExternally(id url) {
		if (!url) return false;
		id scheme = callSimple(url, "scheme");
		auto *s = scheme ? callSimple<const char *>(callSimple(scheme, "lowercaseString"), "UTF8String") : "";
		if (std::strcmp(s, "http") && std::strcmp(s, "https") && std::strcmp(s, "mailto")) return false;
		return callSimple<bool>(callSimple("NSWorkspace", "sharedWorkspace"), "openURL:", url);
	}

	struct ScopedRelease {
		id obj;
		ScopedRelease(id obj) : obj(obj) {}
		~ScopedRelease() {
			if (obj) callVoid(obj, "release");
		}
	};
#define SCOPED_RELEASE(obj) _objc::ScopedRelease release_##obj{obj};
}

struct WebviewGui::Impl {
	WebviewGui *main = nullptr;
	id webview = nullptr;
	id messageHandler = nullptr, schemeHandler = nullptr, uiDelegate = nullptr, navigationDelegate = nullptr,
	   downloadDelegate = nullptr;
	ResourceGetter getter;

	static constexpr const char * associatedObjectKey = "WebviewGui::Impl";

	static void messageHandlerImpl(id self, SEL, id /*controller*/, id message) {
		using namespace _objc;
		auto *impl = (Impl *)objc_getAssociatedObject(self, associatedObjectKey);
		if (!impl || !impl->main) return;

		auto &receiveFn = impl->main->receive;
		if (!receiveFn) return;

		id body = callSimple(message, "body");
		if (instanceOf(body, "NSString")) {
			auto *base64 = callSimple<const char *>(body, "UTF8String");
			auto binary = helpers::decodeBase64(base64);
			receiveFn((const unsigned char *)binary.data(), binary.size());
		}
	}
	
	static id createMessageHandlerClass() {
		using namespace _objc;
		static constexpr const char *className = "WebviewGUI_WKScriptMessageHandler";
		id alreadyRegistered = (id)objc_getClass(className);
		if (alreadyRegistered) return alreadyRegistered;
		
		auto handlerClass = objc_allocateClassPair(objc_getClass("NSObject"), className, 0);
		class_addProtocol(handlerClass, objc_getProtocol("WKScriptMessageHandler"));
		class_addMethod(handlerClass, sel_registerName("userContentController:didReceiveScriptMessage:"), (IMP)messageHandlerImpl, "v@:@@");
		objc_registerClassPair(handlerClass);
		
		return (id)objc_getClass(className);
	}

	// <input type="file">: WebKit shows no file dialog unless the UI delegate does. A modal NSOpenPanel, with the
	// page's multiple / directory flags; cancel answers nil.
	static void openPanelImpl(id /*self*/, SEL, id /*webview*/, id parameters, id /*frame*/, void *completionHandler) {
		using namespace _objc;
		id panel = callSimple("NSOpenPanel", "openPanel");
		const bool multiple = callSimple<bool>(parameters, "allowsMultipleSelection");
		const bool directories = callSimple<bool>(parameters, "allowsDirectories");
		callVoid(panel, "setCanChooseFiles:", (signed char)1);
		callVoid(panel, "setCanChooseDirectories:", (signed char)(directories ? 1 : 0));
		callVoid(panel, "setAllowsMultipleSelection:", (signed char)(multiple ? 1 : 0));
		const long response = callSimple<long>(panel, "runModal");
		id urls = response == 1 /*NSModalResponseOK*/ ? callSimple(panel, "URLs") : nil;
		// call the completion block: void (^)(NSArray<NSURL *> *)
		struct BlockLayout { void *isa; int flags; int reserved; void (*invoke)(void *, id); };
		auto *block = (BlockLayout *)completionHandler;
		if (block && block->invoke) block->invoke(block, urls);
	}
	// window.open() and target="_blank": no window inside the plugin; http(s) links open in the browser
	static id createWebViewImpl(id /*self*/, SEL, id /*webview*/, id /*config*/, id navigationAction, id /*features*/) {
		using namespace _objc;
		openExternally(callSimple(callSimple(navigationAction, "request"), "URL"));
		return nil;
	}
	// alert(), confirm(), prompt(): without these, WebKit returns at once (confirm false, prompt null)
	static id makeAlert(id message) {
		using namespace _objc;
		id alert = callSimple("NSAlert", "new");
		callVoid(alert, "setMessageText:", message ? message : nsString(""));
		callSimple(alert, "addButtonWithTitle:", nsString("OK"));
		return alert;
	}
	static void alertPanelImpl(id /*self*/, SEL, id /*webview*/, id message, id /*frame*/, void *completionHandler) {
		using namespace _objc;
		id alert = makeAlert(message);
		callSimple<long>(alert, "runModal");
		callVoid(alert, "release");
		callBlock(completionHandler);
	}
	static void confirmPanelImpl(id /*self*/, SEL, id /*webview*/, id message, id /*frame*/, void *completionHandler) {
		using namespace _objc;
		id alert = makeAlert(message);
		callSimple(alert, "addButtonWithTitle:", nsString("Cancel"));
		const long response = callSimple<long>(alert, "runModal");
		callVoid(alert, "release");
		callBlock(completionHandler, (signed char)(response == 1000 /*NSAlertFirstButtonReturn*/ ? 1 : 0));
	}
	static void promptPanelImpl(id /*self*/, SEL, id /*webview*/, id prompt, id defaultText, id /*frame*/, void *completionHandler) {
		using namespace _objc;
		id alert = makeAlert(prompt);
		callSimple(alert, "addButtonWithTitle:", nsString("Cancel"));
		id field = callSimple("NSTextField", "alloc");
		CGRect frame{{0, 0}, {280, 24}};
		field = callSimple(field, "initWithFrame:", frame);
		callVoid(field, "setStringValue:", defaultText ? defaultText : nsString(""));
		callVoid(alert, "setAccessoryView:", field);
		callVoid(callSimple(alert, "window"), "setInitialFirstResponder:", field);
		const long response = callSimple<long>(alert, "runModal");
		id text = response == 1000 ? callSimple(callSimple(field, "stringValue"), "copy") : nil;
		callVoid(field, "release");
		callVoid(alert, "release");
		callBlock(completionHandler, text);
		if (text) callVoid(text, "release");
	}
	// Pointer lock (endless knob drags): WKWebView grants it only through this private WKUIDelegatePrivate method
	// (macOS 10.14.4+); without it every request is refused. A plugin's page is its own: allow. If WebKit ever stops
	// calling it, requests are refused as before, and pages fall back to plain drags.
	static void pointerLockImpl(id /*self*/, SEL, id /*webview*/, void *completionHandler) {
		_objc::callBlock(completionHandler, (signed char)1);
	}
	static id createUIDelegateClass() {
		static constexpr const char *className = "WebviewGUI_WKUIDelegate";
		id alreadyRegistered = (id)objc_getClass(className);
		if (alreadyRegistered) return alreadyRegistered;
		auto delegateClass = objc_allocateClassPair(objc_getClass("NSObject"), className, 0);
		class_addProtocol(delegateClass, objc_getProtocol("WKUIDelegate"));
		class_addMethod(delegateClass, sel_registerName("webView:runOpenPanelWithParameters:initiatedByFrame:completionHandler:"), (IMP)openPanelImpl, "v@:@@@@?");
		class_addMethod(delegateClass, sel_registerName("webView:createWebViewWithConfiguration:forNavigationAction:windowFeatures:"), (IMP)createWebViewImpl, "@@:@@@@");
		class_addMethod(delegateClass, sel_registerName("webView:runJavaScriptAlertPanelWithMessage:initiatedByFrame:completionHandler:"), (IMP)alertPanelImpl, "v@:@@@@?");
		class_addMethod(delegateClass, sel_registerName("webView:runJavaScriptConfirmPanelWithMessage:initiatedByFrame:completionHandler:"), (IMP)confirmPanelImpl, "v@:@@@@?");
		class_addMethod(delegateClass, sel_registerName("webView:runJavaScriptTextInputPanelWithPrompt:defaultText:initiatedByFrame:completionHandler:"), (IMP)promptPanelImpl, "v@:@@@@@?");
		class_addMethod(delegateClass, sel_registerName("_webViewDidRequestPointerLock:completionHandler:"), (IMP)pointerLockImpl, "v@:@@?");
		objc_registerClassPair(delegateClass);
		return (id)objc_getClass(className);
	}

	// Downloads (<a download>, blob: URLs, responses the page can't show) become a WKDownload (macOS 11.3+), saved
	// where the user picks in a save panel. Without this, WebKit drops them: an "Export" button did nothing.
	static void decideActionImpl(id /*self*/, SEL, id webview, id action, void *decisionHandler) {
		using namespace _objc;
		if (callSimple<bool>(action, "shouldPerformDownload")) return callBlock(decisionHandler, long(2) /*...PolicyDownload*/);
		// a link the user clicked to another site: the browser, not the plugin's GUI (whose page would be lost)
		id target = callSimple(action, "targetFrame");
		const bool mainFrame = target && callSimple<bool>(target, "isMainFrame");
		if (mainFrame && callSimple<long>(action, "navigationType") == 0 /*WKNavigationTypeLinkActivated*/) {
			id url = callSimple(callSimple(action, "request"), "URL");
			id current = callSimple(webview, "URL");
			auto text = [](id s) { return s ? s : nsString(""); };
			const bool sameSite = current && callSimple<bool>(text(callSimple(url, "scheme")), "isEqualToString:", text(callSimple(current, "scheme")))
			                      && callSimple<bool>(text(callSimple(url, "host")), "isEqualToString:", text(callSimple(current, "host")));
			if (!sameSite && openExternally(url)) return callBlock(decisionHandler, long(0) /*...PolicyCancel*/);
		}
		callBlock(decisionHandler, long(1) /*WKNavigationActionPolicyAllow*/);
	}
	static void decideResponseImpl(id /*self*/, SEL, id /*webview*/, id response, void *decisionHandler) {
		using namespace _objc;
		const bool canShow = callSimple<bool>(response, "canShowMIMEType");
		callBlock(decisionHandler, long(canShow ? 1 /*Allow*/ : 2 /*WKNavigationResponsePolicyDownload*/));
	}
	static void didBecomeDownloadImpl(id self, SEL, id /*webview*/, id /*actionOrResponse*/, id download) {
		using namespace _objc;
		auto *impl = (Impl *)objc_getAssociatedObject(self, associatedObjectKey);
		if (impl && impl->downloadDelegate) callVoid(download, "setDelegate:", impl->downloadDelegate);
	}
	static void downloadDestinationImpl(id /*self*/, SEL, id /*download*/, id /*response*/, id suggestedFilename, void *completionHandler) {
		using namespace _objc;
		id panel = callSimple("NSSavePanel", "savePanel");
		if (suggestedFilename) callVoid(panel, "setNameFieldStringValue:", suggestedFilename);
		const long result = callSimple<long>(panel, "runModal");
		id url = result == 1 /*NSModalResponseOK*/ ? callSimple(panel, "URL") : nil;
		// WKDownload fails if the file exists; the panel has already asked whether to replace it
		if (url) callSimple<bool>(callSimple("NSFileManager", "defaultManager"), "removeItemAtURL:error:", url, (id *)nullptr);
		callBlock(completionHandler, url);
	}
	static id createNavigationDelegateClass() {
		static constexpr const char *className = "WebviewGUI_WKNavigationDelegate";
		id alreadyRegistered = (id)objc_getClass(className);
		if (alreadyRegistered) return alreadyRegistered;
		auto delegateClass = objc_allocateClassPair(objc_getClass("NSObject"), className, 0);
		class_addProtocol(delegateClass, objc_getProtocol("WKNavigationDelegate"));
		class_addMethod(delegateClass, sel_registerName("webView:decidePolicyForNavigationAction:decisionHandler:"), (IMP)decideActionImpl, "v@:@@@?");
		class_addMethod(delegateClass, sel_registerName("webView:decidePolicyForNavigationResponse:decisionHandler:"), (IMP)decideResponseImpl, "v@:@@@?");
		class_addMethod(delegateClass, sel_registerName("webView:navigationAction:didBecomeDownload:"), (IMP)didBecomeDownloadImpl, "v@:@@@");
		class_addMethod(delegateClass, sel_registerName("webView:navigationResponse:didBecomeDownload:"), (IMP)didBecomeDownloadImpl, "v@:@@@");
		objc_registerClassPair(delegateClass);
		return (id)objc_getClass(className);
	}
	static id createDownloadDelegateClass() {
		static constexpr const char *className = "WebviewGUI_WKDownloadDelegate";
		id alreadyRegistered = (id)objc_getClass(className);
		if (alreadyRegistered) return alreadyRegistered;
		auto delegateClass = objc_allocateClassPair(objc_getClass("NSObject"), className, 0);
		if (auto *protocol = objc_getProtocol("WKDownloadDelegate")) class_addProtocol(delegateClass, protocol);
		class_addMethod(delegateClass, sel_registerName("download:decideDestinationUsingResponse:suggestedFilename:completionHandler:"), (IMP)downloadDestinationImpl, "v@:@@@@?");
		objc_registerClassPair(delegateClass);
		return (id)objc_getClass(className);
	}

	static void schemeHandlerStartImpl(id self, SEL, id /*webview*/, id urlSchemeTask) {
		using namespace _objc;
		auto *impl = (Impl *)objc_getAssociatedObject(self, associatedObjectKey);
		if (!impl || !impl->main) return;
		id request = callSimple(urlSchemeTask, "request");
		id url = callSimple(request, "URL");
		auto *urlStr = callSimple<const char *>(callSimple(url, "absoluteString"), "UTF8String");
		// the path after the origin host (using `path` or similar would remove a trailing `/`)
		auto *pathStr = urlStr + std::strlen("webview-gui://");
		if (const char *slash = std::strchr(pathStr, '/')) pathStr = slash;
		Resource resource;
		resource.mediaType = helpers::guessMediaType(pathStr);
		if (!impl->getter(pathStr, resource)) {
			id response = callSimple("NSHTTPURLResponse", "alloc");
			SCOPED_RELEASE(response);
			response = callSimple(
				response,
				"initWithURL:statusCode:HTTPVersion:headerFields:",
				url,
				long(404),
				nsString("HTTP/1.1"),
				nil
			);
			callSimple(urlSchemeTask, "didReceiveResponse:", response);
			callSimple(urlSchemeTask, "didFinish");
			return;
		}

		id headerNames[] = {nsString("Content-Length"), nsString("Content-Type"), nsString("Cache-Control"), nsString("Access-Control-Allow-Origin")};
		auto lengthStr = std::to_string(resource.bytes.size());
		id headerValues[] = {nsString(lengthStr.c_str()), nsString(resource.mediaType.c_str()), nsString("no-store"), nsString("*")};
		id headers = callSimple("NSDictionary", "dictionaryWithObjects:forKeys:count:", headerValues, headerNames, (unsigned long)4);

		id response = callSimple("NSHTTPURLResponse", "alloc");
		response = callSimple(
			response,
			"initWithURL:statusCode:HTTPVersion:headerFields:",
			url,
			long(200),
			nsString("HTTP/1.1"),
			headers
		);
		SCOPED_RELEASE(response);
		callSimple(urlSchemeTask, "didReceiveResponse:", response);
		id data = callSimple("NSData", "dataWithBytes:length:", (const void *)resource.bytes.data(), (unsigned long)resource.bytes.size());
		callSimple(urlSchemeTask, "didReceiveData:", data);
		callSimple(urlSchemeTask, "didFinish");
	}
	static void schemeHandlerStopImpl(id self, SEL, id /*webview*/, id urlSchemeTask) {
	}
	static id createSchemeHandlerClass() {
		using namespace _objc;
		static constexpr const char *className = "WebviewGUI_WKURLSchemeHandler";
		id alreadyRegistered = (id)objc_getClass(className);
		if (alreadyRegistered) return alreadyRegistered;
		
		auto handlerClass = objc_allocateClassPair(objc_getClass("NSObject"), className, 0);
		class_addProtocol(handlerClass, objc_getProtocol("WKURLSchemeHandler"));
		class_addMethod(handlerClass, sel_registerName("webView:startURLSchemeTask:"), (IMP)schemeHandlerStartImpl, "v@:@@");
		class_addMethod(handlerClass, sel_registerName("webView:stopURLSchemeTask:"), (IMP)schemeHandlerStopImpl, "v@:@@");
		objc_registerClassPair(handlerClass);
		
		return (id)objc_getClass(className);
	}

	Impl(ResourceGetter g={}) {
		getter = std::move(g);

		using namespace _objc;
		static id messageHandlerClass = createMessageHandlerClass();
		
		id config = callSimple("WKWebViewConfiguration", "new");
		if (!config) return;
		// Wait until everything's ready until showing anything
		callVoid(config, "setSuppressesIncrementalRendering:", nsNumber(true));
		
		if (getter) {
			static id schemeHandlerClass = createSchemeHandlerClass();
			schemeHandler = callSimple(schemeHandlerClass, "new");
			objc_setAssociatedObject(schemeHandler, associatedObjectKey, (id)this, OBJC_ASSOCIATION_ASSIGN);
			callSimple(config, "setURLSchemeHandler:forURLScheme:", schemeHandler, nsString("webview-gui"));
		}
		
		id preferences = callSimple(config, "preferences");
		callVoid(preferences, "setElementFullscreenEnabled:", nsNumber(false));
		// Require click/etc. to open popups
		callVoid(preferences, "setJavaScriptCanOpenWindowsAutomatically:", nsNumber(false));
		
		id contentController = callSimple(config, "userContentController");
		const char *initJs = R"JS(
			if (!Uint8Array.prototype.toBase64) {
				Uint8Array.prototype.toBase64 = function() {
					let binaryString = "";
					for (var i = 0; i < this.length; i++) {
						binaryString += String.fromCharCode(this[i]);
					}
					return btoa(binaryString);
				};
			}
			if (!Uint8Array.fromBase64) {
				Uint8Array.fromBase64 = b64 => {
					let binaryString = atob(b64);
					let array = new Uint8Array(binaryString.length);
					for (let i=0; i < array.length; ++i) {
						array[i] = binaryString.charCodeAt(i);
					}
					return array;
				};
			}
			window.addEventListener('message', e=>{
				// Outgoing: `window.parent.postMessage()` from the main frame arrives here with source == window.
				// Incoming messages (WebviewGui::send) carry source == window too, as the spec's window.parent, so
				// pages that check event.source accept them. They're synthetic (isTrusted false); postMessage's are not.
				if (e.source == window && e.isTrusted) {
					let data = e.data;
					if (data instanceof ArrayBuffer) data = new Uint8Array(data);
					else if (ArrayBuffer.isView(data)) data = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
					else return; // not a binary frame (e.g. a page's own JSON debug message): leave it to the page
					e.stopImmediatePropagation();
					window.webkit.messageHandlers.webviewGui_receive.postMessage(data.toBase64());
				}
			}, {capture: true});
			// A plugin GUI is not a web page: no browser context menu (the page's own handlers still get the event
			// and can draw their menus), and no scroll bars. The document itself doesn't scroll (a page a few pixels
			// larger than its view shouldn't wobble); scrollable panels inside it still scroll, without bars.
			window.addEventListener('contextmenu', e => e.preventDefault(), {capture: true});
			(() => {
				const style = document.createElement('style');
				style.textContent = 'html, body { overflow: hidden !important; } * { scrollbar-width: none !important; }'
					+ ' ::-webkit-scrollbar { display: none !important; width: 0 !important; height: 0 !important; }';
				(document.head || document.documentElement).appendChild(style);
			})();
		)JS";
		id initScript = callSimple("WKUserScript", "alloc");
		SCOPED_RELEASE(initScript);
		if (initScript) initScript = callSimple(initScript, "initWithSource:injectionTime:forMainFrameOnly:", nsString(initJs), int(0)/*WKUserScriptInjectionTimeAtDocumentStart*/, true);
		if (!initScript) return;
		callSimple(contentController, "addUserScript:", initScript);
		
		messageHandler = callSimple(messageHandlerClass, "new");
		objc_setAssociatedObject(messageHandler, associatedObjectKey, (id)this, OBJC_ASSOCIATION_ASSIGN);
		callSimple(contentController, "addScriptMessageHandler:name:", messageHandler, nsString("webviewGui_receive"));
		callVoid(messageHandler, "release");
		
		webview = callSimple("WKWebView", "alloc");
		CGRect frame{{0, 0}, {100, 100}};
		if (webview) webview = callSimple(webview, "initWithFrame:configuration:", frame, config);
		if (webview) {
			// the webview holds its UI delegate weakly: we keep it
			static id uiDelegateClass = createUIDelegateClass();
			uiDelegate = callSimple(uiDelegateClass, "new");
			callVoid(webview, "setUIDelegate:", uiDelegate);
			// navigation and downloads: held here too (WebKit holds both delegates weakly)
			static id navigationDelegateClass = createNavigationDelegateClass();
			static id downloadDelegateClass = createDownloadDelegateClass();
			navigationDelegate = callSimple(navigationDelegateClass, "new");
			downloadDelegate = callSimple(downloadDelegateClass, "new");
			objc_setAssociatedObject(navigationDelegate, associatedObjectKey, (id)this, OBJC_ASSOCIATION_ASSIGN);
			callVoid(webview, "setNavigationDelegate:", navigationDelegate);
		}
	}
	
	~Impl() {
		using namespace _objc;
		if (messageHandler) objc_setAssociatedObject(messageHandler, associatedObjectKey, (id)nullptr, OBJC_ASSOCIATION_ASSIGN);
		if (schemeHandler) objc_setAssociatedObject(schemeHandler, associatedObjectKey, (id)nullptr, OBJC_ASSOCIATION_ASSIGN);
		if (navigationDelegate) objc_setAssociatedObject(navigationDelegate, associatedObjectKey, (id)nullptr, OBJC_ASSOCIATION_ASSIGN);
		if (webview) {
			callVoid(webview, "setUIDelegate:", (id)nullptr);
			callVoid(webview, "setNavigationDelegate:", (id)nullptr);
			callVoid(webview, "removeFromSuperview");
			callVoid(webview, "release");
		}
		if (uiDelegate) callVoid(uiDelegate, "release");
		if (navigationDelegate) callVoid(navigationDelegate, "release");
		if (downloadDelegate) callVoid(downloadDelegate, "release"); // a download in progress keeps no pointer to us
	}
};

bool WebviewGui::supports(Platform p) {
	return p == Platform::COCOA;
}
WebviewGui * WebviewGui::create(Platform platform, const std::string &startPath, ResourceGetter getter) {
	return create(platform, startPath, std::move(getter), std::string());
}
WebviewGui * WebviewGui::create(Platform platform, const std::string &startPath, ResourceGetter getter, const std::string &originHost) {
	if (!supports(platform)) return nullptr;
	
	using namespace _objc;
	// webview-gui://<host>/: the host makes the page's origin, so each host gets its own storage
	const std::string base = "webview-gui://" + originHost + "/";
	id baseUrl = callSimple("NSURL", "URLWithString:", nsString(base.c_str()));
	if (!baseUrl) return nullptr;
	id url = callSimple("NSURL", "URLWithString:relativeToURL:", nsString(startPath.c_str()), baseUrl);
	auto *request = _objc::callSimple("NSMutableURLRequest", "requestWithURL:", url);
	
	auto *impl = new Impl(std::move(getter));
	if (!impl->webview) {
		delete impl;
		return nullptr;
	}
	callSimple(impl->webview, "loadRequest:", request);
	return new WebviewGui(impl);
}
WebviewGui * WebviewGui::create(Platform platform, const std::string &startUrl) {
	if (!supports(platform)) return nullptr;

	using namespace _objc;
	id url = callSimple("NSURL", "URLWithString:", nsString(startUrl.c_str()));
	if (!url) return nullptr;

	auto *impl = new Impl();
	if (!impl->webview) {
		delete impl;
		return nullptr;
	}
	auto *request = _objc::callSimple("NSMutableURLRequest", "requestWithURL:", url);
	callSimple(impl->webview, "loadRequest:", request);
	return new WebviewGui(impl);
}
WebviewGui * WebviewGui::create(Platform platform, const std::string &startPathOrUrl, const std::string &baseDir) {
	if (!baseDir.size()) return create(platform, startPathOrUrl);
	if (!supports(platform)) return nullptr;

	using namespace _objc;
	id baseUrl = callSimple("NSURL", "fileURLWithPath:isDirectory:", nsString(baseDir.c_str()), nsNumber(true));
	if (!baseUrl) return nullptr;
	
	auto *startUrlC = startPathOrUrl.c_str();
	if (startUrlC[0] == '/') ++startUrlC; // skip any leading `/`, since we have a base directory
	id url = callSimple("NSURL", "URLWithString:relativeToURL:", nsString(startUrlC), baseUrl);
	if (!url) return nullptr;
	
	auto *impl = new Impl();
	if (!impl->webview) {
		delete impl;
		return nullptr;
	}

	if (callSimple<bool>(url, "isFileURL")) {
		callSimple(impl->webview, "loadFileURL:allowingReadAccessToURL:", url, baseUrl);
	} else {
		auto *request = _objc::callSimple("NSMutableURLRequest", "requestWithURL:", url);
		callSimple(impl->webview, "loadRequest:", request);
	}
	return new WebviewGui(impl);
}

WebviewGui::WebviewGui(WebviewGui::Impl *impl) : impl(impl) {
	impl->main = this;
}
WebviewGui::~WebviewGui() {
	delete impl;
}
void WebviewGui::attach(void *platformNative) {
	using namespace _objc;
	callVoid((id)platformNative, "addSubview:", impl->webview);
}
void WebviewGui::send(const unsigned char *bytes, size_t length) {
	// source: window, which is window.parent for the main frame (pages may check event.source === window.parent)
	std::string js = "window.dispatchEvent(new MessageEvent('message',{source:window,data:Uint8Array.fromBase64('";
	helpers::encodeBase64(bytes, length, js);
	js += "').buffer}))";

	using namespace _objc;
	callSimple(impl->webview, "evaluateJavaScript:completionHandler:", nsString(js.c_str()), (id)nullptr);
}
void WebviewGui::setSize(double width, double height) {
	using namespace _objc;
	CGRect rect{{0, 0}, {width, height}};
	callSimple(impl->webview, "setFrame:", rect);
}
void WebviewGui::setVisible(bool visible) {}

} // namespace

#undef SCOPED_RELEASE
