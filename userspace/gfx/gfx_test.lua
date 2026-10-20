local pass, fail = 0, 0

local function check(name, cond)
    if cond then
        pass = pass + 1
        print("PASS " .. name)
        gl.log("PASS " .. name)
    else
        fail = fail + 1
        print("FAIL " .. name)
        gl.log("FAIL " .. name)
    end
end

print("=== LuOS 0.10.56 gfx (Lua) - CPU Mesa rendering test ===")

local cl, err = gl.start()
check("gl.start", cl ~= nil)
if not cl then
    print("start error: " .. tostring(err))
    return
end

local renderer = tostring(gl.string(gl.RENDERER))
local version = tostring(gl.string(gl.VERSION))
print("renderer: " .. renderer)
print("version:  " .. version)
print("vendor:   " .. tostring(gl.string(gl.VENDOR)))
print("GLSL:     " .. tostring(gl.string(gl.SHADING_LANGUAGE_VERSION)))
check("renderer is llvmpipe", renderer:find("llvmpipe") ~= nil)
check("GL version >= 2.1", version:find("2%.1") ~= nil or version:find("3%.") ~= nil or version:find("4%.") ~= nil)
check("initial glGetError clean", gl.getError() == 0)

local w, h = gl.open(128, 128)
if not w then
    print("gl.open error: " .. tostring(h))
    gl.log("gl.open error: " .. tostring(h))
end
check("gl.open 128x128", w == 128 and h == 128)

gl.clearColor(0.2, 0.4, 0.8, 1.0)
gl.clear(gl.COLOR_BUFFER_BIT)
local pix = gl.readPixels(0, 0, 128, 128)
check("readPixels size", pix ~= nil and #pix == 128 * 128 * 4)
local r0, g0, b0, a0 = gl.pixel(pix, 64, 64, 128)
print(string.format("clear pixel: %d %d %d %d", r0, g0, b0, a0))
gl.log(string.format("clear pixel %d %d %d %d err=%d", r0, g0, b0, a0, gl.getError()))
check("clear color match", math.abs(r0 - 51) <= 2 and math.abs(g0 - 102) <= 2 and math.abs(b0 - 204) <= 2 and a0 == 255)
local r1, g1, b1 = gl.pixel(pix, 5, 7, 128)
check("clear color uniform", math.abs(r1 - 51) <= 2 and math.abs(g1 - 102) <= 2 and math.abs(b1 - 204) <= 2)
check("crc of clear frame nonzero", gl.crc(pix) ~= 0)
print(string.format("frame crc32: 0x%08X", gl.crc(pix)))

gl.clearColor(0, 0, 0, 1)
gl.clear(gl.COLOR_BUFFER_BIT)
gl.matrixMode(gl.PROJECTION)
gl.loadIdentity()
gl.ortho(-1, 1, -1, 1, -1, 1)
gl.matrixMode(gl.MODELVIEW)
gl.loadIdentity()
gl.begin(gl.TRIANGLES)
gl.color3f(1, 0, 0)
gl.vertex2f(-0.6, -0.6)
gl.color3f(1, 0, 0)
gl.vertex2f(0.6, -0.6)
gl.color3f(1, 0, 0)
gl.vertex2f(0, 0.6)
gl.End()
pix = gl.readPixels(0, 0, 128, 128)
local tr, tg, tb, ta = gl.pixel(pix, 64, 40, 128)
print(string.format("triangle pixel: %d %d %d %d", tr, tg, tb, ta))
gl.log(string.format("tri pixel %d %d %d %d", tr, tg, tb, ta))
check("immediate-mode triangle", tr > 240 and tg < 15 and tb < 15 and ta == 255)
local br, bg, bb = gl.pixel(pix, 8, 8, 128)
check("triangle background", br < 15 and bg < 15 and bb < 15)
check("immediate-mode glGetError clean", gl.getError() == 0)

gl.begin(gl.QUADS)
gl.color3f(0, 1, 0)
gl.vertex2f(-0.9, -0.9)
gl.vertex2f(-0.2, -0.9)
gl.vertex2f(-0.2, -0.2)
gl.vertex2f(-0.9, -0.2)
gl.End()
pix = gl.readPixels(0, 0, 128, 128)
local qr, qg, qb = gl.pixel(pix, 14, 14, 128)
print(string.format("quad pixel: %d %d %d", qr, qg, qb))
check("immediate-mode quad", qg > 240 and qr < 15 and qb < 15)
check("quad did not erase triangle", gl.pixel(pix, 64, 40, 128) > 240)

gl.clearColor(0, 0, 0, 1)
gl.clear(gl.COLOR_BUFFER_BIT)
gl.matrixMode(gl.MODELVIEW)
gl.loadIdentity()
gl.rotatef(45, 0, 0, 1)
gl.begin(gl.TRIANGLES)
gl.color3f(0, 0.5, 1)
gl.vertex2f(-0.5, -0.5)
gl.vertex2f(0.5, -0.5)
gl.vertex2f(0, 0.5)
gl.End()
pix = gl.readPixels(0, 0, 128, 128)
local rr, rg2, rb = gl.pixel(pix, 64, 64, 128)
print(string.format("rotated triangle center: %d %d %d", rr, rg2, rb))
check("rotated triangle center pixel", rr < 15 and rg2 > 100 and rb > 240)
check("glRotatef glGetError clean", gl.getError() == 0)

local vs_src = "#version 120\n" ..
    "attribute vec2 pos;\n" ..
    "varying vec2 uv;\n" ..
    "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n"
local fs_src = "#version 120\n" ..
    "varying vec2 uv;\n" ..
    "void main() { gl_FragColor = vec4(uv.x, uv.y, 0.25, 1.0); }\n"

local vs, vserr = gl.createShader(gl.VERTEX_SHADER, vs_src)
if not vs then gl.log("vs err: " .. tostring(vserr)) end
check("vertex shader compiled", vs ~= nil)
local fs, fserr = gl.createShader(gl.FRAGMENT_SHADER, fs_src)
if not fs then gl.log("fs err: " .. tostring(fserr)) end
check("fragment shader compiled", fs ~= nil)
local bad = gl.createShader(gl.FRAGMENT_SHADER, "#version 120\nvoid main() { gl_FragColor = vec4(bogus; }\n")
check("bad shader rejected with log", bad == nil)

local prog = gl.createProgram(vs, fs, {"pos"})
check("program linked", prog ~= nil)

local vbo = gl.genBuffers(1)
check("genBuffers", vbo ~= nil and vbo > 0)
gl.bindBuffer(gl.ARRAY_BUFFER, vbo)
gl.bufferData(gl.ARRAY_BUFFER, {-1, -1, 1, -1, -1, 1, 1, 1}, gl.STATIC_DRAW)
gl.useProgram(prog)
gl.enableVertexAttribArray(0)
gl.vertexAttribPointer(0, 2, gl.FLOAT, 0, 0, 0)
gl.clearColor(0, 0, 0, 1)
gl.clear(gl.COLOR_BUFFER_BIT)
gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4)
pix = gl.readPixels(0, 0, 128, 128)
local ur, ug, ub, ua = gl.pixel(pix, 122, 122, 128)
print(string.format("shader gradient pixel: %d %d %d %d", ur, ug, ub, ua))
check("shader gradient top-right", ur > 230 and ug > 230 and math.abs(ub - 64) <= 4 and ua == 255)
local zr, zg, zb = gl.pixel(pix, 2, 2, 128)
check("shader gradient bottom-left", zr < 20 and zg < 20 and math.abs(zb - 64) <= 4)
check("shader pipeline glGetError clean", gl.getError() == 0)
gl.useProgram(0)

local tex = gl.genTextures(1)
check("genTextures", tex ~= nil and tex > 0)
gl.bindTexture(gl.TEXTURE_2D, tex)
gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST)
gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST)
local texels = {}
for y = 1, 8 do
    for x = 1, 8 do
        local v = (x <= 4) and 255 or 40
        texels[#texels + 1] = string.char(v, 255 - v, 32, 255)
    end
end
local tdata = table.concat(texels)
local tr2 = gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 8, 8, gl.RGBA, gl.UNSIGNED_BYTE, tdata)
check("texImage2D returns 0", tr2 == 0)
check("texture upload glGetError clean", gl.getError() == 0)

gl.matrixMode(gl.PROJECTION)
gl.loadIdentity()
gl.perspective(60, 1, 0.1, 100)
gl.matrixMode(gl.MODELVIEW)
gl.loadIdentity()
gl.translatef(0.1, 0.2, 0)
check("matrix stack glGetError clean", gl.getError() == 0)

gl.matrixMode(gl.MODELVIEW)
gl.loadIdentity()
gl.lookAt(0, 0, 3, 0, 0, 0, 0, 1, 0)
check("lookAt glGetError clean", gl.getError() == 0)

gl.clearColor(0.1, 0.1, 0.1, 1)
gl.clear(gl.COLOR_BUFFER_BIT)
gl.begin(gl.LINES)
gl.color3f(1, 1, 0)
gl.vertex2f(-0.5, -0.5)
gl.vertex2f(0.5, 0.5)
gl.End()
gl.begin(gl.POINTS)
gl.color3f(0, 1, 1)
gl.vertex2f(0, 0)
gl.End()
check("lines and points glGetError clean", gl.getError() == 0)

local fin = gl.finish()
check("glFinish", fin == 0)

local ok, perr = gl.present()
print("present: " .. tostring(ok) .. " " .. tostring(perr))
check("present frame to screen", ok == true)

check("final glGetError clean", gl.getError() == 0)

gl.close()
check("gl.close", true)

print(string.format("RESULT: %d passed, %d failed", pass, fail))
gl.log(string.format("RESULT %d passed %d failed", pass, fail))
if fail == 0 then
    print("GFX_LUA_TEST_OK")
    gl.log("GFX_LUA_TEST_OK")
else
    print("GFX_LUA_TEST_FAILED")
    gl.log("GFX_LUA_TEST_FAILED")
end
