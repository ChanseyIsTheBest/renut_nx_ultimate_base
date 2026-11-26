# ilomilo-nx (2026-11-25): every output word of the first vertices a draw's geometry stage emitted (run inside
# qrenderdoc, like tools/rdc_mesh.py): to compare the corners a rectangle-list geometry shader makes with the ones it
# was given. RDC_IN = capture, RDC_OUT = folder for gsout.txt, RDC_EVENTS = "e1,...", RDC_VERTS = vertices (default 6),
# RDC_WORDS = floats a vertex (default 40), RDC_STAGE = "gs" (default) or "vs".
import os
import struct
import traceback

import renderdoc as rd

IN = os.environ.get("RDC_IN")
OUT = os.environ.get("RDC_OUT")
EVENTS = [int(e) for e in os.environ.get("RDC_EVENTS", "").split(",") if e.strip()]
VERTS = int(os.environ.get("RDC_VERTS", "6"))
WORDS = int(os.environ.get("RDC_WORDS", "40"))
STAGE = rd.MeshDataStage.GSOut if os.environ.get("RDC_STAGE", "gs") == "gs" else rd.MeshDataStage.VSOut
log = open(os.path.join(OUT, "gsout.txt"), "w")


def say(*parts):
    log.write(" ".join(str(p) for p in parts) + "\n")
    log.flush()


try:
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(IN, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("open file")
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    status, controller = result if isinstance(result, tuple) else (rd.ResultCode.Succeeded, result)
    for ev in EVENTS:
        controller.SetFrameEvent(ev, True)
        mesh = controller.GetPostVSData(0, 0, STAGE)
        say("event", ev, "stride", mesh.vertexByteStride, "vertices", mesh.numIndices)
        if mesh.vertexResourceId == rd.ResourceId.Null():
            say("   no data")
            continue
        data = controller.GetBufferData(mesh.vertexResourceId, mesh.vertexByteOffset, 0)
        stride = mesh.vertexByteStride
        words = min(WORDS, stride // 4)
        for v in range(min(VERTS, len(data) // stride)):
            vals = struct.unpack_from("%df" % words, data, v * stride)
            say("  v%d:" % v, " ".join("%.4g" % x for x in vals))
    controller.Shutdown()
    cap.Shutdown()
    say("done")
except Exception:
    say("ERROR", traceback.format_exc())
finally:
    log.close()
    os._exit(0)
