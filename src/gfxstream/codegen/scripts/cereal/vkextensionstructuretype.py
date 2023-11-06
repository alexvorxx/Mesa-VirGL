# Copyright (c) 2022 The Android Open Source Project
# Copyright (c) 2022 Google Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from .wrapperdefs import VulkanWrapperGenerator


class VulkanExtensionStructureType(VulkanWrapperGenerator):
    def __init__(self, extensionName: str, module, typeInfo):
        super().__init__(module, typeInfo)
        self._extensionName = extensionName

    def onGenGroup(self, groupinfo, groupName, alias=None):
        super().onGenGroup(groupinfo, groupName, alias)
        elem = groupinfo.elem
        if (not elem.get('type') == 'enum'):
            return
        if (not elem.get('name') == 'VkStructureType'):
            return
        extensionEnumFactoryMacro = f'{self._extensionName.upper()}_ENUM'
        for enum in elem.findall(f"enum[@extname='{self._extensionName}']"):
            name = enum.get('name')
            offset = enum.get('offset')
            self.module.appendHeader(
                f"#define {name} {extensionEnumFactoryMacro}(VkStructureType, {offset})\n")


class VulkanGfxstreamStructureType(VulkanExtensionStructureType):
    def __init__(self, module, typeInfo):
        super().__init__('VK_GOOGLE_gfxstream', module, typeInfo)


class VulkanAndroidNativeBufferStructureType(VulkanExtensionStructureType):
    def __init__(self, module, typeInfo):
        super().__init__('VK_ANDROID_native_buffer', module, typeInfo)
