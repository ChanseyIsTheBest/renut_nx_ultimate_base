# ilomilo-nx (2026-11-25): the multisample and rasterizer state of chosen draws of a Vulkan RenderDoc capture, every
# field RenderDoc has (the blur draws' pixel history said "sampleMasked" where no fragment showed), and the geometry
# stage's execution modes from its disassembly (input / output topology, vertex count).
#   RDC_IN = capture, RDC_OUT = folder for ms.txt, RDC_EVENTS = "e1,e2,..."
import os
import traceback

import renderdoc as rd

IN = os.environ.get("RDC_IN")
OUT = os.environ.get("RDC_OUT")
EVENTS = {int(e) for e in os.environ.get("RDC_EVENTS", "").split(",") if e.strip()}
log = open(os.path.join(OUT, "ms.txt"), "w")


def say(*parts):
    log.write(" ".join(str(p) for p in parts) + "\n")
    log.flush()


def flatten(actions, out):
    for a in actions:
        out.append(a)
        flatten(a.children, out)
    return out


def dump(name, obj):
    vals = []
    for n in dir(obj):
        if n.startswith("_"):
            continue
        try:
            v = getattr(obj, n)
        except Exception:
            continue
        if callable(v):
            continue
        vals.append("%s=%s" % (n, v))
    say("   " + name, " ".join(vals))


try:
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(IN, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("open file")
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    status, controller = result if isinstance(result, tuple) else (rd.ResultCode.Succeeded, result)
    targets = controller.GetDisassemblyTargets(True)
    for a in flatten(controller.GetRootActions(), []):
        if a.eventId not in EVENTS:
            continue
        controller.SetFrameEvent(a.eventId, True)
        say("event", a.eventId, "indices", a.numIndices, "instances", a.numInstances)
        vk = controller.GetVulkanPipelineState()
        dump("multisample", vk.multisample)
        dump("rasterizer", vk.rasterizer)
        dump("inputAssembly", vk.inputAssembly)
        try:
            dump("colorBlend", vk.colorBlend)
        except Exception as e:
            say("   colorBlend ?", e)
        try:
            fb = vk.currentPass.framebuffer
            for i, att in enumerate(fb.attachments):
                tex = controller.GetTexture(att.resource) if hasattr(controller, "GetTexture") else None
                say("   attachment", i, att.resource, att.viewFormat.Name() if hasattr(att, "viewFormat") else "")
        except Exception as e:
            say("   framebuffer ?", e)
        state = controller.GetPipelineState()
        for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Geometry, rd.ShaderStage.Pixel):
            refl = state.GetShaderReflection(stage)
            if refl is None:
                continue
            pipe = state.GetGraphicsPipelineObject()
            text = controller.DisassembleShader(pipe, refl, targets[0] if targets else "")
            lines = [l for l in text.splitlines() if "ExecutionMode" in l or "OpEntryPoint" in l or
                     "SampleMask" in l or "Kill" in l or "OpDemote" in l or "Invocations" in l or
                     "OutputVertices" in l or "Output" in l and "Triangle" in l]
            say("   stage", stage, "entry", refl.entryPoint, "lines", len(text.splitlines()))
            for l in lines[:40]:
                say("      ", l.strip())
            with open(os.path.join(OUT, "ev%d_%s.txt" % (a.eventId, str(stage).split(".")[-1])), "w") as f:
                f.write(text)
    controller.Shutdown()
    cap.Shutdown()
    say("done")
except Exception:
    say("ERROR", traceback.format_exc())
log.close()
os._exit(0)
