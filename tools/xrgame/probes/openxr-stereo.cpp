// SPDX-License-Identifier: GPL-3.0-or-later
// Bounded Windows x64 D3D11 -> OpenXR -> Android stereo bridge probe.
// Loads our runtime directly to isolate the bridge from third-party loader/game failures.
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_NO_PROTOTYPES
#include <windows.h>
#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cmath>
static FILE* logFile;
static void check(XrResult r, const char* call) {
    if (XR_FAILED(r)) { fprintf(logFile, "FAIL %s %d\n", call, r); fflush(logFile); ExitProcess(2); }
}
#define XR(call) check(call, #call)
#define PROC(name) PFN_##name name; XR(get(instance, #name, reinterpret_cast<PFN_xrVoidFunction*>(&name)))
int main(int argc, char** argv) {
    bool readPixels = argc > 1 && !strcmp(argv[1], "--readback");
    CreateDirectoryA("C:\\gamenative-xr", nullptr);
    logFile = fopen("C:\\gamenative-xr\\stereo-probe.log", "w");
    if (!logFile) return 1;
    setvbuf(logFile, nullptr, _IONBF, 0);
    fprintf(logFile, "BEGIN pid=%lu\n", GetCurrentProcessId());
    HMODULE dll = LoadLibraryA("C:\\gamenative-xr\\gamenative_openxr_runtime64.dll");
    if (!dll) { fprintf(logFile, "LoadLibrary error=%lu\n", GetLastError()); return 1; }
    auto get = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(dll,"xrGetInstanceProcAddr"));
    if (!get) return 1;
    XrInstance instance = XR_NULL_HANDLE;
    PROC(xrCreateInstance);
    const char* extensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ici.applicationInfo.applicationName, "XRGame stereo probe");
    ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    ici.enabledExtensionCount = 1; ici.enabledExtensionNames = extensions;
    XR(xrCreateInstance(&ici, &instance));
    PROC(xrGetSystem); PROC(xrGetD3D11GraphicsRequirementsKHR); PROC(xrCreateSession);
    PROC(xrEnumerateViewConfigurationViews); PROC(xrCreateReferenceSpace);
    PROC(xrCreateSwapchain); PROC(xrEnumerateSwapchainImages);
    PROC(xrPollEvent); PROC(xrBeginSession); PROC(xrWaitFrame); PROC(xrBeginFrame);
    PROC(xrLocateViews); PROC(xrAcquireSwapchainImage); PROC(xrWaitSwapchainImage);
    PROC(xrReleaseSwapchainImage); PROC(xrEndFrame); PROC(xrRequestExitSession);
    PROC(xrEndSession); PROC(xrDestroySwapchain); PROC(xrDestroySpace);
    PROC(xrDestroySession); PROC(xrDestroyInstance);
    PROC(xrStringToPath); PROC(xrCreateActionSet); PROC(xrCreateAction);
    PROC(xrSuggestInteractionProfileBindings); PROC(xrAttachSessionActionSets);
    PROC(xrCreateActionSpace); PROC(xrSyncActions); PROC(xrGetActionStateFloat);
    PROC(xrGetActionStateVector2f); PROC(xrGetActionStateBoolean); PROC(xrLocateSpace);
    PROC(xrDestroyActionSet);
    XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO}; gi.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system; XR(xrGetSystem(instance,&gi,&system));
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XR(xrGetD3D11GraphicsRequirementsKHR(instance,system,&req));
    ID3D11Device* device=nullptr; ID3D11DeviceContext* context=nullptr;
    const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_0};
    HRESULT hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels,1,D3D11_SDK_VERSION,&device,nullptr,&context);
    if (FAILED(hr)) { fprintf(logFile,"D3D11CreateDevice hr=%lx\n",hr); return 2; }
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR}; binding.device=device;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO}; sci.next=&binding;sci.systemId=system;
    XrSession session; XR(xrCreateSession(instance,&sci,&session));
    XrViewConfigurationView config[2]={{XR_TYPE_VIEW_CONFIGURATION_VIEW},{XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t count; XR(xrEnumerateViewConfigurationViews(instance,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,2,&count,config));
    if(count!=2) return 2;
    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_STAGE;spaceInfo.poseInReferenceSpace.orientation.w=1;
    XrSpace space; XR(xrCreateReferenceSpace(session,&spaceInfo,&space));
    // Exercise the real guest action API, not only the host control socket.
    auto path = [&](const char* name) { XrPath value; XR(xrStringToPath(instance,name,&value)); return value; };
    XrPath hands[2]={path("/user/hand/left"),path("/user/hand/right")};
    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(asci.actionSetName,"probe");strcpy(asci.localizedActionSetName,"Probe");
    XrActionSet actionSet;XR(xrCreateActionSet(instance,&asci,&actionSet));
    const char* names[]={"trigger","grip","stick","primary","thumbclick","menu","pose"};
    XrActionType types[]={XR_ACTION_TYPE_FLOAT_INPUT,XR_ACTION_TYPE_FLOAT_INPUT,XR_ACTION_TYPE_VECTOR2F_INPUT,
        XR_ACTION_TYPE_BOOLEAN_INPUT,XR_ACTION_TYPE_BOOLEAN_INPUT,XR_ACTION_TYPE_BOOLEAN_INPUT,XR_ACTION_TYPE_POSE_INPUT};
    const char* suffixes[]={"/input/trigger/value","/input/squeeze/value","/input/thumbstick",
        nullptr,"/input/thumbstick/click","/input/menu/click","/input/grip/pose"};
    XrAction actions[7];std::vector<XrActionSuggestedBinding> bindings;
    for(int a=0;a<7;++a){
        XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};strcpy(ci.actionName,names[a]);strcpy(ci.localizedActionName,names[a]);
        ci.actionType=types[a];ci.countSubactionPaths=2;ci.subactionPaths=hands;XR(xrCreateAction(actionSet,&ci,&actions[a]));
        for(int hand=0;hand<2;++hand){
            if(a==5 && hand==1)continue;
            char bindingPath[128];snprintf(bindingPath,sizeof(bindingPath),"/user/hand/%s%s",hand?"right":"left",
                a==3?(hand?"/input/a/click":"/input/x/click"):suffixes[a]);
            bindings.push_back({actions[a],path(bindingPath)});
        }
    }
    XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile=path("/interaction_profiles/oculus/touch_controller");
    suggested.countSuggestedBindings=uint32_t(bindings.size());suggested.suggestedBindings=bindings.data();
    XR(xrSuggestInteractionProfileBindings(instance,&suggested));
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};attach.countActionSets=1;attach.actionSets=&actionSet;
    XR(xrAttachSessionActionSets(session,&attach));
    XrSpace handSpaces[2];
    for(int hand=0;hand<2;++hand){
        XrActionSpaceCreateInfo ci{XR_TYPE_ACTION_SPACE_CREATE_INFO};ci.action=actions[6];ci.subactionPath=hands[hand];ci.poseInActionSpace.orientation.w=1;
        XR(xrCreateActionSpace(session,&ci,&handSpaces[hand]));
    }
    XrSwapchain chains[2]; std::vector<XrSwapchainImageD3D11KHR> images[2];
    std::vector<ID3D11RenderTargetView*> targets[2];
    for(int e=0;e<2;++e){
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;ci.format=DXGI_FORMAT_R8G8B8A8_UNORM;
        ci.sampleCount=1;ci.width=config[e].recommendedImageRectWidth;ci.height=config[e].recommendedImageRectHeight;
        ci.faceCount=1;ci.arraySize=1;ci.mipCount=1;
        XR(xrCreateSwapchain(session,&ci,&chains[e]));
        XR(xrEnumerateSwapchainImages(chains[e],0,&count,nullptr));
        images[e].resize(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});targets[e].resize(count);
        XR(xrEnumerateSwapchainImages(chains[e],count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images[e].data())));
        for(uint32_t i=0;i<count;++i){
            hr=device->CreateRenderTargetView(images[e][i].texture,nullptr,&targets[e][i]);
            if(FAILED(hr)){fprintf(logFile,"CreateRTV hr=%lx\n",hr);return 2;}
        }
        fprintf(logFile,"eye=%d size=%ux%u images=%u\n",e,ci.width,ci.height,count);
    }
    bool running=false,done=false,exitRequested=false; unsigned frames=0;
    ULONGLONG start=GetTickCount64();
    while(!done && GetTickCount64()-start<120000){
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while(xrPollEvent(instance,&event)==XR_SUCCESS){
            if(event.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED){
                auto state=reinterpret_cast<XrEventDataSessionStateChanged*>(&event)->state;
                fprintf(logFile,"state=%d\n",state);
                if(state==XR_SESSION_STATE_READY){
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};bi.primaryViewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    XR(xrBeginSession(session,&bi));running=true;
                }
                if(state==XR_SESSION_STATE_STOPPING){XR(xrEndSession(session));running=false;done=true;}
                if(state==XR_SESSION_STATE_EXITING || state==XR_SESSION_STATE_LOSS_PENDING) done=true;
            }
            event={XR_TYPE_EVENT_DATA_BUFFER};
        }
        if(done)break;
        if(!running){Sleep(10);continue;}
        XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO}; XrFrameState fs{XR_TYPE_FRAME_STATE};
        XR(xrWaitFrame(session,&wi,&fs));XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};XR(xrBeginFrame(session,&bi));
        XrActiveActionSet active{actionSet,XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};sync.countActiveActionSets=1;sync.activeActionSets=&active;XR(xrSyncActions(session,&sync));
        if(frames%30==0)for(int hand=0;hand<2;++hand){
            XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};getInfo.subactionPath=hands[hand];
            XrActionStateFloat trigger{XR_TYPE_ACTION_STATE_FLOAT},grip{XR_TYPE_ACTION_STATE_FLOAT};
            XrActionStateVector2f stick{XR_TYPE_ACTION_STATE_VECTOR2F};
            XrActionStateBoolean primary{XR_TYPE_ACTION_STATE_BOOLEAN},click{XR_TYPE_ACTION_STATE_BOOLEAN},menu{XR_TYPE_ACTION_STATE_BOOLEAN};
            getInfo.action=actions[0];XR(xrGetActionStateFloat(session,&getInfo,&trigger));
            getInfo.action=actions[1];XR(xrGetActionStateFloat(session,&getInfo,&grip));
            getInfo.action=actions[2];XR(xrGetActionStateVector2f(session,&getInfo,&stick));
            getInfo.action=actions[3];XR(xrGetActionStateBoolean(session,&getInfo,&primary));
            getInfo.action=actions[4];XR(xrGetActionStateBoolean(session,&getInfo,&click));
            getInfo.action=actions[5];XR(xrGetActionStateBoolean(session,&getInfo,&menu));
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};XR(xrLocateSpace(handSpaces[hand],space,fs.predictedDisplayTime,&location));
            const auto& p=location.pose;
            fprintf(logFile,"input frame=%u hand=%d active=%u trigger=%.3f grip=%.3f stick=%.3f,%.3f primary=%u click=%u menu=%u flags=%llu pose=%.3f,%.3f,%.3f q=%.3f,%.3f,%.3f,%.3f\n",
                frames,hand,trigger.isActive,trigger.currentState,grip.currentState,stick.currentState.x,stick.currentState.y,
                primary.currentState,click.currentState,menu.currentState,(unsigned long long)location.locationFlags,
                p.position.x,p.position.y,p.position.z,p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w);
        }
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};li.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime=fs.predictedDisplayTime;li.space=space;
        XrViewState vs{XR_TYPE_VIEW_STATE};XrView views[2]={{XR_TYPE_VIEW},{XR_TYPE_VIEW}};
        XR(xrLocateViews(session,&li,&vs,2,&count,views));
        XrCompositionLayerProjectionView pv[2]={{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        for(int e=0;e<2;++e){
            uint32_t index;XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};XR(xrAcquireSwapchainImage(chains[e],&ai,&index));
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;XR(xrWaitSwapchainImage(chains[e],&wait));
            const float color[4]={e==0?0.85f:0.08f,e==1?0.85f:0.08f,0.1f+0.2f*float(frames%120)/120.f,1.f};
            context->ClearRenderTargetView(targets[e][index],color);
            if (readPixels && frames == 0) {
                D3D11_TEXTURE2D_DESC desc{}; images[e][index].texture->GetDesc(&desc);
                desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                ID3D11Texture2D* readback=nullptr;
                hr=device->CreateTexture2D(&desc,nullptr,&readback);
                if(SUCCEEDED(hr)){
                    context->CopyResource(readback,images[e][index].texture);
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    hr=context->Map(readback,0,D3D11_MAP_READ,0,&mapped);
                    if(SUCCEEDED(hr)){
                        auto* pixel=static_cast<unsigned char*>(mapped.pData);
                        fprintf(logFile,"source eye=%d RGBA=%u,%u,%u,%u (one-time diagnostic readback)\n",e,pixel[0],pixel[1],pixel[2],pixel[3]);
                        context->Unmap(readback,0);
                    }
                    readback->Release();
                }
            }
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};XR(xrReleaseSwapchainImage(chains[e],&ri));
            pv[e].pose=views[e].pose;pv[e].fov=views[e].fov;pv[e].subImage.swapchain=chains[e];
            pv[e].subImage.imageRect.extent={int32_t(config[e].recommendedImageRectWidth),int32_t(config[e].recommendedImageRectHeight)};
        }
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.space=space;projection.viewCount=2;projection.views=pv;
        const XrCompositionLayerBaseHeader* layers[]={reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};ei.displayTime=fs.predictedDisplayTime;ei.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;ei.layerCount=fs.shouldRender?1:0;ei.layers=layers;
        XR(xrEndFrame(session,&ei));++frames;
        if(frames==1 || frames%60==0)fprintf(logFile,"frame=%u time=%lld render=%u leftX=%f rightX=%f q=%f,%f,%f,%f\n",frames,fs.predictedDisplayTime,fs.shouldRender,views[0].pose.position.x,views[1].pose.position.x,views[0].pose.orientation.x,views[0].pose.orientation.y,views[0].pose.orientation.z,views[0].pose.orientation.w);
        if(frames>=3600 && !exitRequested){XR(xrRequestExitSession(session));exitRequested=true;}
    }
    for(int e=0;e<2;++e){for(auto* target:targets[e])target->Release();XR(xrDestroySwapchain(chains[e]));}
    for(auto handSpace:handSpaces)XR(xrDestroySpace(handSpace));
    XR(xrDestroyActionSet(actionSet));
    XR(xrDestroySpace(space));XR(xrDestroySession(session));XR(xrDestroyInstance(instance));
    context->Release();device->Release();
    fprintf(logFile,"END frames=%u duration_ms=%llu clean_stop=%d\n",frames,GetTickCount64()-start,done);
    fclose(logFile);return done && frames>=3600?0:3;
}
