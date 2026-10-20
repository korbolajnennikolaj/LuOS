import vk

npass = 0
nfail = 0


def check(name, cond):
    global npass, nfail
    if cond:
        npass += 1
        print("PASS " + name)
        vk.log("PASS " + name)
    else:
        nfail += 1
        print("FAIL " + name)
        vk.log("FAIL " + name)


print("=== LuOS 0.10.56 Vulkan (MicroPython) - llvmpipe test ===")

r = vk.start()
check("vk.start", r is not None and not isinstance(r, tuple))

if isinstance(r, tuple):
    print("start error: " + str(r[1]))
else:
    ver = vk.instanceVersion()
    if isinstance(ver, tuple):
        print("instance version error: " + str(ver[1]))
    else:
        print("instance version: {}.{}.{} (raw 0x{:X})".format(ver["major"], ver["minor"], ver["patch"], ver["raw"]))
        check("vkEnumerateInstanceVersion", ver["major"] >= 1)
        check("Vulkan 1.1+", ver["major"] > 1 or ver["minor"] >= 1)

    res = vk.createInstance("LuOS gfx vk test")
    inst = None if isinstance(res, tuple) else res
    check("vkCreateInstance", inst is not None)

    if inst is not None:
        res = vk.enumeratePhysicalDevices(inst)
        if isinstance(res, tuple):
            devs, count = res
        else:
            devs, count = res, 1
        check("vkEnumeratePhysicalDevices", count >= 1 and devs is not None and len(devs) >= 1)

        if devs and len(devs) >= 1:
            props = vk.physicalDeviceProperties(devs[0])
            if isinstance(props, tuple):
                print("properties error: " + str(props[1]))
            else:
                print("device:    " + str(props["deviceName"]))
                av = props["apiVersion"]
                print("api:       {}.{}.{}".format(av["major"], av["minor"], av["patch"]))
                print("vendor:    0x{:X}  device: 0x{:X}  type: {}".format(props["vendorID"], props["deviceID"], props["deviceType"]))
                print("maxTex2D:  " + str(props["maxImageDimension2D"]))
                check("physical device properties", len(str(props["deviceName"])) > 0)
                check("device is llvmpipe CPU", "llvmpipe" in str(props["deviceName"]) or
                      props["deviceType"] == vk.PHYSICAL_DEVICE_TYPE_CPU)
                check("api version >= 1.1", av["major"] > 1 or (av["major"] == 1 and av["minor"] >= 1))

            qf = vk.queueFamilyProperties(devs[0])
            if isinstance(qf, tuple):
                print("queue family error: " + str(qf[1]))
            else:
                print("queue families: {} flags=0x{:X} count={} graphics={} compute={}".format(
                    qf["count"], qf["queueFlags"], qf["queueCount"], qf["graphics"], qf["compute"]))
                check("queue family properties", qf["count"] >= 1)
                check("graphics queue present", qf["graphics"] is True)
                check("compute queue present", qf["compute"] is True)

            mem = vk.memoryProperties(devs[0])
            if isinstance(mem, tuple):
                print("memory properties error: " + str(mem[1]))
            else:
                print("memory types: {} heaps: {}".format(mem["memoryTypeCount"], mem["memoryHeapCount"]))
                host_visible = 0
                for t in mem["types"]:
                    if t["hostVisible"]:
                        host_visible += 1
                print("host visible types: " + str(host_visible))
                check("memory properties", mem["memoryTypeCount"] >= 1 and mem["memoryHeapCount"] >= 1)
                check("host visible memory exists", host_visible >= 1)
                if len(mem["heaps"]) > 0:
                    print("heap 0 size MB: " + str(mem["heaps"][0]["sizeMB"]))
                    check("memory heap nonzero", mem["heaps"][0]["sizeMB"] > 0)

            res = vk.createDevice(devs[0], 0, 1)
            dev = None if isinstance(res, tuple) else res
            check("vkCreateDevice", dev is not None)

            if dev is not None:
                res = vk.getDeviceQueue(dev, 0, 0)
                q = None if isinstance(res, tuple) else res
                check("vkGetDeviceQueue", q is not None and q != 0)
                check("vkDeviceWaitIdle", vk.deviceWaitIdle(dev) == 0)
                vk.destroyDevice(dev)

        res = vk.shaderTest()
        print("built-in vk shader test: rc={} crc=0x{:X}".format(res[0], res[1]))
        check("built-in vk shader test", res[0] == 0 and res[1] != 0)

        vk.destroyInstance(inst)
        check("vk error clean", vk.error() is None)
        vk.close()

print("RESULT: {} passed, {} failed".format(npass, nfail))
vk.log("RESULT {} passed {} failed".format(npass, nfail))
if nfail == 0:
    print("GFX_VK_PY_TEST_OK")
    vk.log("GFX_VK_PY_TEST_OK")
else:
    print("GFX_VK_PY_TEST_FAILED")
    vk.log("GFX_VK_PY_TEST_FAILED")
