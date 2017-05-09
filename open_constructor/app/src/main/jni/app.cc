#include "app.h"

namespace {
    const int kSubdivisionSize = 20000;
    const int kTangoCoreMinimumVersion = 9377;

    void onPointCloudAvailableRouter(void *context, const TangoPointCloud *point_cloud) {
        oc::App *app = static_cast<oc::App *>(context);
        app->onPointCloudAvailable((TangoPointCloud*)point_cloud);
    }

    void onFrameAvailableRouter(void *context, TangoCameraId id, const TangoImageBuffer *buffer) {
        oc::App *app = static_cast<oc::App *>(context);
        app->onFrameAvailable(id, buffer);
    }
}

namespace oc {

    void App::onPointCloudAvailable(TangoPointCloud *point_cloud) {
        if (!t3dr_is_running_)
            return;

        TangoMatrixTransformData matrix_transform;
        TangoSupport_getMatrixTransformAtTime(
                point_cloud->timestamp, TANGO_COORDINATE_FRAME_AREA_DESCRIPTION,
                TANGO_COORDINATE_FRAME_CAMERA_DEPTH, TANGO_SUPPORT_ENGINE_OPENGL,
                TANGO_SUPPORT_ENGINE_TANGO, ROTATION_0, &matrix_transform);
        if (matrix_transform.status_code != TANGO_POSE_VALID)
            return;

        binder_mutex_.lock();
        point_cloud_matrix_ = glm::make_mat4(matrix_transform.matrix);
        TangoSupport_updatePointCloud(tango.Pointcloud(), point_cloud);
        point_cloud_available_ = true;
        binder_mutex_.unlock();
    }

    void App::onFrameAvailable(TangoCameraId id, const TangoImageBuffer *buffer) {
        if (id != TANGO_CAMERA_COLOR || !t3dr_is_running_)
            return;

        TangoMatrixTransformData matrix_transform;
        TangoSupport_getMatrixTransformAtTime(
                        buffer->timestamp, TANGO_COORDINATE_FRAME_AREA_DESCRIPTION,
                        TANGO_COORDINATE_FRAME_CAMERA_COLOR, TANGO_SUPPORT_ENGINE_OPENGL,
                        TANGO_SUPPORT_ENGINE_TANGO, ROTATION_0, &matrix_transform);
        if (matrix_transform.status_code != TANGO_POSE_VALID)
            return;

        binder_mutex_.lock();
        if (!point_cloud_available_) {
            binder_mutex_.unlock();
            return;
        }

        image_matrix = glm::make_mat4(matrix_transform.matrix);
        Tango3DR_ImageBuffer t3dr_image;
        t3dr_image.width = buffer->width;
        t3dr_image.height = buffer->height;
        t3dr_image.stride = buffer->stride;
        t3dr_image.timestamp = buffer->timestamp;
        t3dr_image.format = static_cast<Tango3DR_ImageFormatType>(buffer->format);
        t3dr_image.data = buffer->data;

        Tango3DR_Pose t3dr_image_pose = GLCamera::Extract3DRPose(image_matrix);
        glm::quat rot = glm::quat((float) t3dr_image_pose.orientation[0],
                                  (float) t3dr_image_pose.orientation[1],
                                  (float) t3dr_image_pose.orientation[2],
                                  (float) t3dr_image_pose.orientation[3]);
        float diff = GLCamera::Diff(rot, image_rotation);
        image_rotation = rot;
        if (diff > 1) {
            binder_mutex_.unlock();
            return;
        }

        Tango3DR_PointCloud t3dr_depth;
        TangoSupport_getLatestPointCloud(tango.Pointcloud(), &front_cloud_);
        t3dr_depth.timestamp = front_cloud_->timestamp;
        t3dr_depth.num_points = front_cloud_->num_points;
        t3dr_depth.points = front_cloud_->points;

        Tango3DR_Pose t3dr_depth_pose = GLCamera::Extract3DRPose(point_cloud_matrix_);
        Tango3DR_GridIndexArray t3dr_updated;
        Tango3DR_Status ret;
        ret = Tango3DR_update(tango.Context(), &t3dr_depth, &t3dr_depth_pose,
                              &t3dr_image, &t3dr_image_pose, &t3dr_updated);
        if (ret != TANGO_3DR_SUCCESS)
        {
            binder_mutex_.unlock();
            return;
        }

        texturize.Add(t3dr_image, image_matrix, tango.Dataset());
        std::vector<std::pair<GridIndex, Tango3DR_Mesh*> > added;
        added = scan.Process(tango.Context(), &t3dr_updated);
        render_mutex_.lock();
        scan.Merge(added);
        render_mutex_.unlock();

        Tango3DR_GridIndexArray_destroy(&t3dr_updated);
        point_cloud_available_ = false;
        binder_mutex_.unlock();
    }


    App::App() :  t3dr_is_running_(false),
                  gyro(false),
                  landscape(false),
                  point_cloud_available_(false),
                  zoom(0) {}

    void App::OnCreate(JNIEnv *env, jobject activity) {
        int version;
        TangoErrorType err = TangoSupport_GetTangoVersion(env, activity, &version);
        if (err != TANGO_SUCCESS || version < kTangoCoreMinimumVersion)
            std::exit(EXIT_SUCCESS);
    }

    void App::OnTangoServiceConnected(JNIEnv *env, jobject binder, double res,
               double dmin, double dmax, int noise, bool land, std::string dataset) {
        landscape = land;

        TangoService_setBinder(env, binder);
        tango.SetupConfig(dataset);

        TangoErrorType ret = TangoService_connectOnPointCloudAvailable(onPointCloudAvailableRouter);
        if (ret != TANGO_SUCCESS)
            std::exit(EXIT_SUCCESS);
        ret = TangoService_connectOnFrameAvailable(TANGO_CAMERA_COLOR, this, onFrameAvailableRouter);
        if (ret != TANGO_SUCCESS)
            std::exit(EXIT_SUCCESS);

        binder_mutex_.lock();
        tango.Connect(this);
        tango.Setup3DR(res, dmin, dmax, noise);
        binder_mutex_.unlock();
    }

    void App::OnPause() {
        render_mutex_.lock();
        tango.Disconnect();
        scene.DeleteResources();
        render_mutex_.unlock();
    }

    void App::OnSurfaceCreated() {
        render_mutex_.lock();
        scene.InitGLContent();
        render_mutex_.unlock();
    }

    void App::OnSurfaceChanged(int width, int height) {
        render_mutex_.lock();
        scene.SetupViewPort(width, height);
        render_mutex_.unlock();
    }

    void App::OnDrawFrame() {
        render_mutex_.lock();
        //camera transformation
        if (!gyro) {
            scene.renderer->camera.position = glm::vec3(movex, 0, movey);
            scene.renderer->camera.rotation = glm::quat(glm::vec3(yaw, pitch, 0));
            scene.renderer->camera.scale    = glm::vec3(1, 1, 1);
        } else {
            TangoMatrixTransformData transform;
            TangoSupport_getMatrixTransformAtTime(
                    0, TANGO_COORDINATE_FRAME_AREA_DESCRIPTION, TANGO_COORDINATE_FRAME_DEVICE,
                    TANGO_SUPPORT_ENGINE_OPENGL, TANGO_SUPPORT_ENGINE_OPENGL,
                    landscape ? ROTATION_90 : ROTATION_0, &transform);
            if (transform.status_code == TANGO_POSE_VALID) {
                scene.renderer->camera.SetTransformation(glm::make_mat4(transform.matrix));
                scene.UpdateFrustum(scene.renderer->camera.position, zoom);
            }
        }
        //zoom
        glm::vec4 move = scene.renderer->camera.GetTransformation() * glm::vec4(0, 0, zoom, 0);
        scene.renderer->camera.position += glm::vec3(move.x, move.y, move.z);
        //render
        scene.Render(gyro);
        for (std::pair<GridIndex, Tango3DR_Mesh*> s : scan.Data()) {
            scene.renderer->Render(&s.second->vertices[0][0], 0, 0,
                                   (unsigned int*)&s.second->colors[0][0],
                                   s.second->num_faces * 3, &s.second->faces[0][0]);
        }
        render_mutex_.unlock();
    }

    void App::OnToggleButtonClicked(bool t3dr_is_running) {
        binder_mutex_.lock();
        t3dr_is_running_ = t3dr_is_running;
        binder_mutex_.unlock();
    }

    void App::OnClearButtonClicked() {
        binder_mutex_.lock();
        render_mutex_.lock();
        scan.Clear();
        tango.Clear();
        texturize.Clear();
        render_mutex_.unlock();
        binder_mutex_.unlock();
    }

    void App::Load(std::string filename) {
        binder_mutex_.lock();
        render_mutex_.lock();
        File3d io(filename, false);
        io.ReadModel(kSubdivisionSize, scene.static_meshes_);
        render_mutex_.unlock();
        binder_mutex_.unlock();
    }

    void App::Save(std::string filename, std::string dataset) {
        binder_mutex_.lock();
        render_mutex_.lock();
        if (!dataset.empty()) {
            if (texturize.Init(tango.Context(), dataset)) {
                texturize.ApplyFrames(tango.Dataset()); //TODO:remove after Tango team removes memory leaks from SDK
                texturize.Process(filename);

                //merge with previous OBJ
                //TODO:wait until Tango team removes memory leaks from SDK
                /*scan.Clear();
                tango.Clear();
                File3d(filename, false).ReadModel(kSubdivisionSize, scene.static_meshes_);
                File3d(filename, true).WriteModel(scene.static_meshes_);*/
            }
        }
        render_mutex_.unlock();
        binder_mutex_.unlock();
    }

    void App::Texturize(std::string filename, std::string dataset) {
        binder_mutex_.lock();
        render_mutex_.lock();
        //TODO:wait until Tango team removes memory leaks from SDK
        /*if (!dataset.empty()) {

            if (!texturize.Init(filename, dataset)) {
                render_mutex_.unlock();
                binder_mutex_.unlock();
                return;
            }
            scan.Clear();
            tango.Clear();
            texturize.ApplyFrames(tango.Dataset());
            texturize.Process(filename);
            texturize.Clear();

            //reload the model
            for (unsigned int i = 0; i < scene.static_meshes_.size(); i++)
                scene.static_meshes_[i].Destroy();
            scene.static_meshes_.clear();
            File3d io(filename, false);
            io.ReadModel(kSubdivisionSize, scene.static_meshes_);
        }*/
        //TODO:remove after Tango team removes memory leaks from SDK
        scan.Clear();
        tango.Clear();
        File3d(filename, false).ReadModel(kSubdivisionSize, scene.static_meshes_);
        File3d(filename, true).WriteModel(scene.static_meshes_);
        render_mutex_.unlock();
        binder_mutex_.unlock();
    }
}


static oc::App app;

std::string jstring2string(JNIEnv* env, jstring name)
{
  const char *s = env->GetStringUTFChars(name,NULL);
  std::string str( s );
  env->ReleaseStringUTFChars(name,s);
  return str;
}

#ifdef __cplusplus
extern "C" {
#endif

JNIEXPORT void JNICALL
Java_com_lvonasek_openconstructor_TangoJNINative_onCreate(
JNIEnv* env, jobject, jobject activity) {
  app.OnCreate(env, activity);
}

JNIEXPORT void JNICALL
Java_com_lvonasek_openconstructor_TangoJNINative_onTangoServiceConnected(JNIEnv* env, jobject,
          jobject iBinder, jdouble res, jdouble dmin, jdouble dmax, jint noise, jboolean land,
                                                                              jstring dataset) {
  app.OnTangoServiceConnected(env, iBinder, res, dmin, dmax, noise, land, jstring2string(env, dataset));
}

JNIEXPORT void JNICALL
Java_com_lvonasek_openconstructor_TangoJNINative_onPause(JNIEnv*, jobject) {
  app.OnPause();
}

JNIEXPORT void JNICALL
Java_com_lvonasek_openconstructor_TangoJNINative_onGlSurfaceCreated(JNIEnv*, jobject) {
  app.OnSurfaceCreated();
}

JNIEXPORT void JNICALL
Java_com_lvonasek_openconstructor_TangoJNINative_onGlSurfaceChanged(
    JNIEnv*, jobject, jint width, jint height) {
  app.OnSurfaceChanged(width, height);
}

JNIEXPORT void JNICALL
Java_com_lvonasek_openconstructor_TangoJNINative_onGlSurfaceDrawFrame(JNIEnv*, jobject) {
  app.OnDrawFrame();
}

JNIEXPORT void JNICALL
Java_com_lvonasek_openconstructor_TangoJNINative_onToggleButtonClicked(
    JNIEnv*, jobject, jboolean t3dr_is_running) {
  app.OnToggleButtonClicked(t3dr_is_running);
}

JNIEXPORT void JNICALL
Java_com_lvonasek_openconstructor_TangoJNINative_onClearButtonClicked(JNIEnv*, jobject) {
  app.OnClearButtonClicked();
}

JNIEXPORT void JNICALL
Java_com_lvonasek_openconstructor_TangoJNINative_load(JNIEnv* env, jobject, jstring name) {
  app.Load(jstring2string(env, name));
}

JNIEXPORT void JNICALL
Java_com_lvonasek_openconstructor_TangoJNINative_save(JNIEnv* env, jobject, jstring name, jstring d) {
  app.Save(jstring2string(env, name), jstring2string(env, d));
}

JNIEXPORT void JNICALL
Java_com_lvonasek_openconstructor_TangoJNINative_texturize(JNIEnv* env, jobject, jstring name, jstring d) {
  app.Texturize(jstring2string(env, name), jstring2string(env, d));
}

JNIEXPORT void JNICALL
Java_com_lvonasek_openconstructor_TangoJNINative_setView(JNIEnv*, jobject, jfloat pitch, jfloat yaw,
                                                         jfloat x, jfloat y, jboolean gyro) {
  app.SetView(pitch, yaw, x, y, gyro);
}

JNIEXPORT void JNICALL
Java_com_lvonasek_openconstructor_TangoJNINative_setZoom(JNIEnv*, jobject, jfloat value) {
  app.SetZoom(value);
}

#ifndef NDEBUG
JNIEXPORT jbyteArray JNICALL
Java_com_lvonasek_openconstructor_TangoJNINative_clientSecret(JNIEnv* env, jobject) {
  std::string message = "NO SECRET";
  int byteCount = message.length();
  const jbyte* pNativeMessage = reinterpret_cast<const jbyte*>(message.c_str());
  jbyteArray bytes = env->NewByteArray(byteCount);
  env->SetByteArrayRegion(bytes, 0, byteCount, pNativeMessage);
  return bytes;
}
#else
#include "secret.h"
JNIEXPORT jbyteArray JNICALL
Java_com_lvonasek_openconstructor_TangoJNINative_clientSecret(JNIEnv* env, jobject) {
  std::string message = secret();
  int byteCount = message.length();
  const jbyte* pNativeMessage = reinterpret_cast<const jbyte*>(message.c_str());
  jbyteArray bytes = env->NewByteArray(byteCount);
  env->SetByteArrayRegion(bytes, 0, byteCount, pNativeMessage);
  return bytes;
}
#endif

#ifdef __cplusplus
}
#endif