local pass, fail = 0, 0

local function check(name, cond)
    if cond then
        pass = pass + 1
        print("PASS " .. name)
        vk.log("PASS " .. name)
    else
        fail = fail + 1
        print("FAIL " .. name)
        vk.log("FAIL " .. name)
    end
end

print("=== LuOS 0.10.56 Vulkan (Lua) - llvmpipe test ===")

local cl, err = vk.start()
check("vk.start", cl ~= nil)
if not cl then
    print("start error: " .. tostring(err))
    return
end

local ver = vk.instanceVersion()
print(string.format("instance version: %d.%d.%d (raw 0x%X)",
                    ver.major, ver.minor, ver.patch, ver.raw))
check("vkEnumerateInstanceVersion", ver.major >= 1)
check("Vulkan 1.1+", ver.major > 1 or ver.minor >= 1)

local inst = vk.createInstance("LuOS gfx vk test")
check("vkCreateInstance", inst ~= nil)
if not inst then
    print("createInstance error: " .. tostring(vk.error()))
    return
end

local devs, count = vk.enumeratePhysicalDevices(inst)
check("vkEnumeratePhysicalDevices", count >= 1 and devs ~= nil and #devs >= 1)
if not devs or #devs == 0 then
    print("no physical devices")
    return
end

local props = vk.physicalDeviceProperties(devs[1])
print("device:    " .. tostring(props.deviceName))
print(string.format("api:       %d.%d.%d", props.apiVersion.major, props.apiVersion.minor, props.apiVersion.patch))
print(string.format("vendor:    0x%X  device: 0x%X  type: %d",
                    props.vendorID, props.deviceID, props.deviceType))
print("maxTex2D:  " .. tostring(props.maxImageDimension2D))
check("physical device properties", props.deviceName ~= nil and #tostring(props.deviceName) > 0)
check("device is llvmpipe CPU", tostring(props.deviceName):find("llvmpipe") ~= nil or
      props.deviceType == vk.PHYSICAL_DEVICE_TYPE_CPU)
check("api version >= 1.1", props.apiVersion.major > 1 or
      (props.apiVersion.major == 1 and props.apiVersion.minor >= 1))

local qf = vk.queueFamilyProperties(devs[1])
print(string.format("queue families: %d, flags=0x%X count=%d graphics=%s compute=%s",
                    qf.count, qf.queueFlags, qf.queueCount, tostring(qf.graphics), tostring(qf.compute)))
check("queue family properties", qf.count >= 1)
check("graphics queue present", qf.graphics == true)
check("compute queue present", qf.compute == true)

local mem = vk.memoryProperties(devs[1])
print(string.format("memory types: %d heaps: %d", mem.memoryTypeCount, mem.memoryHeapCount))
local host_visible = 0
for i, t in ipairs(mem.types) do
    if t.hostVisible then host_visible = host_visible + 1 end
end
print("host visible types: " .. host_visible)
check("memory properties", mem.memoryTypeCount >= 1 and mem.memoryHeapCount >= 1)
check("host visible memory exists", host_visible >= 1)
if #mem.heaps > 0 and mem.heaps[1] then
    print("heap 0 size MB: " .. tostring(mem.heaps[1].sizeMB))
    check("memory heap nonzero", (mem.heaps[1].sizeMB or 0) > 0)
end

local dev = vk.createDevice(devs[1], 0, 1)
check("vkCreateDevice", dev ~= nil)
local q = nil
if dev then
    q = vk.getDeviceQueue(dev, 0, 0)
    check("vkGetDeviceQueue", q ~= nil and q ~= 0)
    local idle = vk.deviceWaitIdle(dev)
    check("vkDeviceWaitIdle", idle == 0)
end

local r, crc = vk.shaderTest()
print(string.format("built-in vk shader test: rc=%d crc=0x%X", r, crc))
check("built-in vk shader test", r == 0 and crc ~= 0)

if dev then vk.destroyDevice(dev) end
vk.destroyInstance(inst)
check("vk error clean", vk.error() == nil)
vk.close()

print(string.format("RESULT: %d passed, %d failed", pass, fail))
vk.log(string.format("RESULT %d passed %d failed", pass, fail))
if fail == 0 then
    print("GFX_VK_LUA_TEST_OK")
    vk.log("GFX_VK_LUA_TEST_OK")
else
    print("GFX_VK_LUA_TEST_FAILED")
    vk.log("GFX_VK_LUA_TEST_FAILED")
end
