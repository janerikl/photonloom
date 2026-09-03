cmake_minimum_required(VERSION 3.20)
project(Photonloom LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Optimize by default — the retouch pixel pipelines are far too slow unoptimized.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent Svg)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GPHOTO2 REQUIRED IMPORTED_TARGET libgphoto2)
pkg_check_modules(LIBRAW REQUIRED IMPORTED_TARGET libraw)
pkg_check_modules(LIBZIP REQUIRED IMPORTED_TARGET libzip)
pkg_check_modules(LIBMTP REQUIRED IMPORTED_TARGET libmtp)
pkg_check_modules(LIBTIFF REQUIRED IMPORTED_TARGET libtiff-4)

add_executable(photonloom
    src/main.cpp
    src/camera/CameraWorker.cpp
    src/camera/CameraController.cpp
    src/device/MtpWorker.cpp
    src/device/MtpController.cpp
    src/capture/SessionManager.cpp
    src/capture/NefPreview.cpp
    src/ui/TetherView.cpp
    src/ui/LiveViewWidget.cpp
    src/ui/PannableScrollArea.cpp
    src/ui/ControlsPanel.cpp
    src/ui/PreferencesDialog.cpp
    src/ui/ShortcutRegistry.cpp
    src/ui/FilmstripWidget.cpp
    src/ui/HistogramWidget.cpp
    src/ui/LevelsPanel.cpp
    src/ui/MaskPanel.cpp
    src/ui/LayersPanel.cpp
    src/ui/LayerAdjustmentsPanel.cpp
    src/ui/TonePanel.cpp
    src/ui/ColorPanel.cpp
    src/ui/ToneCurvePanel.cpp
    src/ui/DetailEffectsPanel.cpp
    src/ui/ColorSwatchWidget.cpp
    src/ui/ToolFlyout.cpp
    src/ui/AppIcon.cpp
    src/ui/BrushPresetMenuButton.cpp
    src/ui/AssetsPanel.cpp
    src/ui/BrowseTab.cpp
    src/edit/Adjustments.cpp
    src/edit/RawLoader.cpp
    src/edit/RawMetadata.cpp
    src/edit/ImageCanvas.cpp
    src/edit/RetouchTab.cpp
    src/edit/RetouchWindow.cpp
    src/edit/ExportPreset.cpp
    src/edit/AdjustmentPreset.cpp
    src/edit/ViewTemplate.cpp
    src/edit/BrushPreset.cpp
    src/edit/AssetStamp.cpp
    src/edit/RecentSessions.cpp
    src/edit/RecentFiles.cpp
    src/edit/RecentProjects.cpp
    src/edit/ExportDialog.cpp
    src/edit/TiffExport.cpp
    src/edit/CurveEditor.cpp
    src/edit/EditSidecar.cpp
    src/edit/ZipFile.cpp
    src/edit/HealTool.cpp
    src/edit/TextTool.cpp
    src/edit/ShapeTool.cpp
    src/edit/InpaintTool.cpp
    src/edit/NewDocumentDialog.cpp
    src/edit/NewDocumentSize.cpp
    src/svg/SvgEditorTab.cpp
    src/svg/SvgCanvas.cpp
    src/svg/SvgRender.cpp
    src/svg/SvgFileIO.cpp
)

target_include_directories(photonloom PRIVATE src)
target_link_libraries(photonloom PRIVATE
    Qt6::Widgets Qt6::Concurrent Qt6::Svg PkgConfig::GPHOTO2 PkgConfig::LIBRAW PkgConfig::LIBZIP PkgConfig::LIBMTP PkgConfig::LIBTIFF)

# `sudo cmake --install build` copies the binary to ${CMAKE_INSTALL_PREFIX}/bin
# (default /usr/local/bin).
include(GNUInstallDirs)
install(TARGETS photonloom RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Desktop launcher entry.
install(FILES packaging/photonloom.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

# Generate the launcher PNG from the same drawing code as the window icon.
# Uses the offscreen Qt platform so it works on headless installs.
install(CODE "
    set(ICON_DIR \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps\")
    file(MAKE_DIRECTORY \"\${ICON_DIR}\")
    execute_process(
        COMMAND \${CMAKE_COMMAND} -E env QT_QPA_PLATFORM=offscreen
                \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/photonloom\" --export-icon \"\${ICON_DIR}/photonloom.png\"
        RESULT_VARIABLE _rc
    )
    if(NOT _rc EQUAL 0)
        message(WARNING \"Failed to generate photonloom.png (code \${_rc})\")
    endif()
")

# Register CameraSettings metatypes are header-only; nothing extra needed.

enable_testing()
add_executable(af_mapping_test tests/AfMappingTest.cpp)
target_include_directories(af_mapping_test PRIVATE src)
add_test(NAME af_mapping_test COMMAND af_mapping_test)
add_executable(cam_models_test tests/CameraModelsTest.cpp)
target_include_directories(cam_models_test PRIVATE src)
add_test(NAME cam_models_test COMMAND cam_models_test)
add_executable(grid_overlay_test tests/GridOverlayTest.cpp)
target_include_directories(grid_overlay_test PRIVATE src)
add_test(NAME grid_overlay_test COMMAND grid_overlay_test)
add_executable(adjustments_paint_test tests/AdjustmentsPaintTest.cpp src/edit/Adjustments.cpp src/edit/EditSidecar.cpp src/edit/ZipFile.cpp src/edit/ShapeTool.cpp src/edit/TextTool.cpp)
target_include_directories(adjustments_paint_test PRIVATE src)
target_link_libraries(adjustments_paint_test PRIVATE Qt6::Widgets PkgConfig::LIBZIP)
# The project forces a Release build (NDEBUG) by default, which would compile
# out this test's assert()s; force them on regardless of build type.
target_compile_options(adjustments_paint_test PRIVATE -UNDEBUG)
add_test(NAME adjustments_paint_test COMMAND adjustments_paint_test)
add_executable(new_document_size_test tests/NewDocumentSizeTest.cpp src/edit/NewDocumentSize.cpp)
target_include_directories(new_document_size_test PRIVATE src)
target_link_libraries(new_document_size_test PRIVATE Qt6::Widgets)
target_compile_options(new_document_size_test PRIVATE -UNDEBUG)
add_test(NAME new_document_size_test COMMAND new_document_size_test)

add_executable(layer_reorder_test
    tests/LayerReorderTest.cpp
    src/edit/Adjustments.cpp
    src/edit/RawLoader.cpp
    src/edit/ImageCanvas.cpp
    src/edit/RetouchTab.cpp
    src/edit/EditSidecar.cpp
    src/edit/ZipFile.cpp
    src/edit/HealTool.cpp
    src/edit/TextTool.cpp
    src/edit/ShapeTool.cpp
    src/edit/InpaintTool.cpp
)
target_include_directories(layer_reorder_test PRIVATE src)
target_link_libraries(layer_reorder_test PRIVATE Qt6::Widgets Qt6::Concurrent Qt6::Svg PkgConfig::LIBRAW PkgConfig::LIBZIP)
target_compile_options(layer_reorder_test PRIVATE -UNDEBUG)
add_test(NAME layer_reorder_test COMMAND layer_reorder_test)
set_tests_properties(layer_reorder_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(export_tiff_test tests/ExportTiffTest.cpp src/edit/TiffExport.cpp)
target_include_directories(export_tiff_test PRIVATE src)
target_link_libraries(export_tiff_test PRIVATE Qt6::Widgets PkgConfig::LIBTIFF)
target_compile_options(export_tiff_test PRIVATE -UNDEBUG)
add_test(NAME export_tiff_test COMMAND export_tiff_test)
set_tests_properties(export_tiff_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
