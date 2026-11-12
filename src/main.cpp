#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <glad/glad.h>

#include <cglm/cglm.h>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

// Ozz-animation headers (math headers must precede containers using ozz types)
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/containers/vector.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>

// Custom SDL3 stream wrapper for ozz-animation archives
class OzzSDLStream : public ozz::io::Stream
{
public:
    OzzSDLStream(SDL_IOStream *io)
        : io_(io)
        , opened_(io != nullptr)
    {
        if (io_)
            SDL_SeekIO(io_, 0, SDL_IO_SEEK_SET);
    }

    ~OzzSDLStream()
    {
        if (io_)
            SDL_CloseIO(io_);
    }

    bool opened() const override { return opened_; }

    size_t Read(void *buffer, size_t size) override
    {
        size_t total = 0;
        while (total < size)
        {
            size_t read = SDL_ReadIO(io_, (uint8_t *)buffer + total, size - total);
            if (read == 0)
                break;
            total += read;
        }
        return total;
    }

    size_t Write(const void *, size_t) override { return 0; }

    int Seek(int offset, Origin origin) override
    {
        SDL_IOWhence whence = SDL_IO_SEEK_SET;
        if (origin == kCurrent)
            whence = SDL_IO_SEEK_CUR;
        else if (origin == kEnd)
            whence = SDL_IO_SEEK_END;

        Sint64 pos = SDL_SeekIO(io_, offset, whence);
        return pos >= 0 ? 0 : -1;
    }

    int Tell() const override
    {
        Sint64 pos = SDL_TellIO(io_);
        return pos >= 0 ? (int)pos : -1;
    }

    size_t Size() const override
    {
        Sint64 size = SDL_GetIOSize(io_);
        return size >= 0 ? (size_t)size : 0;
    }

private:
    SDL_IOStream *io_;
    bool opened_;
};

const char *skinningVertexShaderSource = R"(
#version 330 core
layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec2 aTexCoord;
layout (location = 2) in vec4 aJoints;  // Up to 4 influencing joints per vertex
layout (location = 3) in vec4 aWeights; // Weights corresponding to each joint

out vec2 vTexCoord;

uniform mat4 uMvpMatrix;

const int MAX_JOINTS = 100;
uniform mat4 uJointMatrices[MAX_JOINTS];

void main()
{
    mat4 skinMatrix = 
          aWeights.x * uJointMatrices[int(aJoints.x)]
        + aWeights.y * uJointMatrices[int(aJoints.y)]
        + aWeights.z * uJointMatrices[int(aJoints.z)]
        + aWeights.w * uJointMatrices[int(aJoints.w)];

    vec4 skinnedPos = skinMatrix * vec4(aPosition, 1.0);

    vTexCoord = aTexCoord;
    gl_Position = uMvpMatrix * skinnedPos;
}
)";

const char *skinningFragmentShaderSource = R"(
#version 330 core
in vec2 vTexCoord;
uniform sampler2D ourTexture;
out vec4 fragColor;

void main()
{
    fragColor = texture(ourTexture, vTexCoord);
}
)";

GLuint createShaderProgram(const char *vertexSrc, const char *fragmentSrc)
{
    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &vertexSrc, nullptr);
    glCompileShader(vertexShader);

    GLint success;
    char infoLog[512];
    glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        glGetShaderInfoLog(vertexShader, 512, nullptr, infoLog);
        SDL_Log("Vertex Shader Compilation Failed: %s", infoLog);
    }

    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &fragmentSrc, nullptr);
    glCompileShader(fragmentShader);

    glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        glGetShaderInfoLog(fragmentShader, 512, nullptr, infoLog);
        SDL_Log("Fragment Shader Compilation Failed: %s", infoLog);
    }

    GLuint shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vertexShader);
    glAttachShader(shaderProgram, fragmentShader);
    glLinkProgram(shaderProgram);

    glGetProgramiv(shaderProgram, GL_LINK_STATUS, &success);
    if (!success)
    {
        glGetProgramInfoLog(shaderProgram, 512, nullptr, infoLog);
        SDL_Log("Shader Program Link Failed: %s", infoLog);
    }

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    return shaderProgram;
}

GLuint createTexture(const char *path)
{
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io)
    {
        SDL_Log("Failed to open file stream for %s: %s", path, SDL_GetError());
        return 0;
    }

    SDL_Surface *surface = IMG_Load_IO(io, true);
    if (!surface)
    {
        SDL_Log("Image loading failed: %s", SDL_GetError());
        return 0;
    }

    SDL_Surface *optimizedSurface = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_ABGR8888);
    SDL_DestroySurface(surface);

    if (!optimizedSurface)
    {
        SDL_Log("Surface conversion failed: %s", SDL_GetError());
        return 0;
    }

    GLuint textureID;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_2D, textureID);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, optimizedSurface->w, optimizedSurface->h,
        0, GL_RGBA, GL_UNSIGNED_BYTE, optimizedSurface->pixels);
    glGenerateMipmap(GL_TEXTURE_2D);

    SDL_DestroySurface(optimizedSurface);

    return textureID;
}

struct InverseBindMatrix
{
    mat4 m;
};

struct AppState
{
    SDL_Window *window = nullptr;
    SDL_GLContext glContext = nullptr;
    GLuint shaderProgram = 0;
    GLuint VAO = 0;
    GLuint VBO_pos = 0;
    GLuint VBO_uv = 0;
    GLuint VBO_joints = 0;
    GLuint VBO_weights = 0;
    GLuint EBO = 0;
    GLuint textureID = 0;
    GLsizei indexCount = 0;
    GLenum indexType = GL_UNSIGNED_SHORT;
    GLint mvpLoc = -1;
    GLint texLoc = -1;
    GLint jointMatricesLoc = -1;

    // Ozz-animation variables
    ozz::animation::Skeleton ozzSkeleton;
    ozz::animation::Animation ozzAnimation;
    ozz::animation::SamplingJob::Context ozzSamplingContext;
    float ozzAnimationTime = 0.0f;
    ozz::vector<ozz::math::Float4x4> ozzModelMatrices;
    std::vector<InverseBindMatrix> ybotInverseBindMatrices;

    Uint64 lastTicks = 0;
};

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        SDL_Log("SDL Initialization failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, "60");

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    AppState *state = new AppState();
    *appstate = state;

    state->window = SDL_CreateWindow("SDL3 ozz-animation Y-Bot Model (cglm)", 800, 450, SDL_WINDOW_OPENGL);
    if (!state->window)
    {
        SDL_Log("Window creation failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    state->glContext = SDL_GL_CreateContext(state->window);
    if (!state->glContext)
    {
        SDL_Log("OpenGL context creation failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    SDL_GL_MakeCurrent(state->window, state->glContext);

    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress))
    {
        SDL_Log("Failed to initialize GLAD");
        return SDL_APP_FAILURE;
    }

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    state->shaderProgram = createShaderProgram(skinningVertexShaderSource, skinningFragmentShaderSource);

    // Load Ozz Skeleton
    const char *skeletonPath = "assets/models/y-bot/skeleton.ozz";
    SDL_IOStream *skeletonIo = SDL_IOFromFile(skeletonPath, "rb");
    if (skeletonIo)
    {
        OzzSDLStream stream(skeletonIo);
        ozz::io::IArchive archive(&stream);
        if (archive.TestTag<ozz::animation::Skeleton>())
        {
            ozz::animation::Skeleton tempSkeleton;
            archive >> tempSkeleton;
            state->ozzSkeleton = std::move(tempSkeleton);
        }
        else
        {
            SDL_Log("CRITICAL: Archive tag mismatch for Skeleton!");
        }
    }
    else
    {
        SDL_Log("Failed to open ozz skeleton: %s", skeletonPath);
        return SDL_APP_FAILURE;
    }

    // Load Ozz Animation
    const char *animationPath = "assets/models/y-bot/animation.ozz";
    SDL_IOStream *animIo = SDL_IOFromFile(animationPath, "rb");
    if (animIo)
    {
        OzzSDLStream stream(animIo);
        ozz::io::IArchive archive(&stream);
        archive >> state->ozzAnimation;
    }
    else
    {
        SDL_Log("Failed to open ozz animation: %s", animationPath);
        return SDL_APP_FAILURE;
    }

    state->ozzSamplingContext.Resize(state->ozzSkeleton.num_joints());
    state->ozzAnimationTime = 0.0f;
    state->ozzModelMatrices.resize(state->ozzSkeleton.num_joints());

    const char *modelPath = "assets/models/y-bot/y-bot.glb";
    size_t glbSize = 0;
    void *glbData = SDL_LoadFile(modelPath, &glbSize);

    if (!glbData)
    {
        SDL_Log("Failed to load Y-Bot model file into memory: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    cgltf_options options = {};
    cgltf_data *data = nullptr;
    cgltf_result result = cgltf_parse(&options, glbData, glbSize, &data);

    if (result == cgltf_result_success)
    {
        cgltf_load_buffers(&options, data, modelPath);

        cgltf_mesh &mesh = data->meshes[0];
        cgltf_primitive &prim = mesh.primitives[0];

        cgltf_accessor *posAccessor = nullptr;
        cgltf_accessor *uvAccessor = nullptr;
        cgltf_accessor *jointsAccessor = nullptr;
        cgltf_accessor *weightsAccessor = nullptr;
        cgltf_accessor *indexAccessor = prim.indices;

        for (cgltf_size i = 0; i < prim.attributes_count; ++i)
        {
            if (prim.attributes[i].type == cgltf_attribute_type_position)
            {
                posAccessor = prim.attributes[i].data;
            }
            else if (prim.attributes[i].type == cgltf_attribute_type_texcoord)
            {
                uvAccessor = prim.attributes[i].data;
            }
            else if (prim.attributes[i].type == cgltf_attribute_type_joints)
            {
                jointsAccessor = prim.attributes[i].data;
            }
            else if (prim.attributes[i].type == cgltf_attribute_type_weights)
            {
                weightsAccessor = prim.attributes[i].data;
            }
        }

        glGenVertexArrays(1, &state->VAO);
        glBindVertexArray(state->VAO);

        if (posAccessor)
        {
            float *positions = (float *)((uint8_t *)posAccessor->buffer_view->buffer->data +
                                         posAccessor->buffer_view->offset +
                                         posAccessor->offset);
            size_t posSize = posAccessor->count * 3 * sizeof(float);
            float *positionsCopy = (float *)SDL_malloc(posSize);
            memcpy(positionsCopy, positions, posSize);

            glGenBuffers(1, &state->VBO_pos);
            glBindBuffer(GL_ARRAY_BUFFER, state->VBO_pos);
            glBufferData(GL_ARRAY_BUFFER, posSize, positionsCopy, GL_STATIC_DRAW);
            SDL_free(positionsCopy);

            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *)0);
        }

        if (uvAccessor)
        {
            float *uvs = (float *)((uint8_t *)uvAccessor->buffer_view->buffer->data +
                                   uvAccessor->buffer_view->offset +
                                   uvAccessor->offset);
            size_t uvSize = uvAccessor->count * 2 * sizeof(float);
            float *uvsCopy = (float *)SDL_malloc(uvSize);
            memcpy(uvsCopy, uvs, uvSize);

            glGenBuffers(1, &state->VBO_uv);
            glBindBuffer(GL_ARRAY_BUFFER, state->VBO_uv);
            glBufferData(GL_ARRAY_BUFFER, uvSize, uvsCopy, GL_STATIC_DRAW);
            SDL_free(uvsCopy);

            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);
        }

        if (jointsAccessor)
        {
            uint8_t *jointsData = (uint8_t *)jointsAccessor->buffer_view->buffer->data +
                                  jointsAccessor->buffer_view->offset +
                                  jointsAccessor->offset;
            size_t jointsSize = jointsAccessor->count * jointsAccessor->stride;
            void *jointsCopy = SDL_malloc(jointsSize);
            memcpy(jointsCopy, jointsData, jointsSize);

            glGenBuffers(1, &state->VBO_joints);
            glBindBuffer(GL_ARRAY_BUFFER, state->VBO_joints);
            glBufferData(GL_ARRAY_BUFFER, jointsSize, jointsCopy, GL_STATIC_DRAW);
            SDL_free(jointsCopy);

            GLenum jointGlType = GL_UNSIGNED_SHORT;
            size_t componentSize = sizeof(unsigned short);
            if (jointsAccessor->component_type == cgltf_component_type_r_8u)
            {
                jointGlType = GL_UNSIGNED_BYTE;
                componentSize = sizeof(unsigned char);
            }

            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 4, jointGlType, GL_FALSE, 4 * componentSize, (void *)0);
        }

        if (weightsAccessor)
        {
            float *weightsData = (float *)((uint8_t *)weightsAccessor->buffer_view->buffer->data +
                                           weightsAccessor->buffer_view->offset +
                                           weightsAccessor->offset);
            size_t weightsSize = weightsAccessor->count * 4 * sizeof(float);
            float *weightsCopy = (float *)SDL_malloc(weightsSize);
            memcpy(weightsCopy, weightsData, weightsSize);

            glGenBuffers(1, &state->VBO_weights);
            glBindBuffer(GL_ARRAY_BUFFER, state->VBO_weights);
            glBufferData(GL_ARRAY_BUFFER, weightsSize, weightsCopy, GL_STATIC_DRAW);
            SDL_free(weightsCopy);

            glEnableVertexAttribArray(3);
            glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
        }

        if (indexAccessor)
        {
            void *indices = (void *)((uint8_t *)indexAccessor->buffer_view->buffer->data +
                                     indexAccessor->buffer_view->offset +
                                     indexAccessor->offset);
            size_t indexSize = indexAccessor->count * indexAccessor->stride;
            void *indicesCopy = SDL_malloc(indexSize);
            memcpy(indicesCopy, indices, indexSize);

            glGenBuffers(1, &state->EBO);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, state->EBO);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, indexSize, indicesCopy, GL_STATIC_DRAW);
            SDL_free(indicesCopy);

            if (indexAccessor->component_type == cgltf_component_type_r_32u)
            {
                state->indexType = GL_UNSIGNED_INT;
            }
            else if (indexAccessor->component_type == cgltf_component_type_r_16u)
            {
                state->indexType = GL_UNSIGNED_SHORT;
            }
            else if (indexAccessor->component_type == cgltf_component_type_r_8u)
            {
                state->indexType = GL_UNSIGNED_BYTE;
            }

            state->indexCount = (GLsizei)indexAccessor->count;
        }

        glBindVertexArray(0);

        if (data->skins_count > 0)
        {
            cgltf_skin &skin = data->skins[0];
            if (skin.inverse_bind_matrices)
            {
                cgltf_accessor *ibmAccessor = skin.inverse_bind_matrices;
                size_t numJointsInSkin = skin.joints_count;
                state->ybotInverseBindMatrices.resize(numJointsInSkin);
                float *ibmData = (float *)((uint8_t *)ibmAccessor->buffer_view->buffer->data +
                                           ibmAccessor->buffer_view->offset +
                                           ibmAccessor->offset);
                for (size_t i = 0; i < numJointsInSkin; ++i)
                {
                    memcpy(state->ybotInverseBindMatrices[i].m, &ibmData[i * 16], 16 * sizeof(float));
                }
            }
        }

        cgltf_free(data);
        SDL_free(glbData);
    }
    else
    {
        SDL_Log("Failed to parse Y-Bot GLB model: %s", modelPath);
        SDL_free(glbData);
        return SDL_APP_FAILURE;
    }

    state->textureID = createTexture("assets/models/y-bot/y-bot.jpg");
    if (state->textureID == 0)
    {
        SDL_Log("Failed to load Y-Bot JPEG texture.");
        return SDL_APP_FAILURE;
    }

    state->mvpLoc = glGetUniformLocation(state->shaderProgram, "uMvpMatrix");
    state->texLoc = glGetUniformLocation(state->shaderProgram, "ourTexture");
    state->jointMatricesLoc = glGetUniformLocation(state->shaderProgram, "uJointMatrices");
    state->lastTicks = SDL_GetTicksNS();

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    if (event->type == SDL_EVENT_QUIT)
    {
        return SDL_APP_SUCCESS;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    auto *state = static_cast<AppState *>(appstate);

    Uint64 currentTicks = SDL_GetTicksNS();
    float deltaTime = static_cast<float>(currentTicks - state->lastTicks) / 1e9f;
    state->lastTicks = currentTicks;

    if (state->ozzAnimation.duration() > 0.0f)
    {
        state->ozzAnimationTime += deltaTime;
        if (state->ozzAnimationTime > state->ozzAnimation.duration())
        {
            state->ozzAnimationTime = fmod(state->ozzAnimationTime, state->ozzAnimation.duration());
        }

        ozz::animation::SamplingJob sampling_job;
        sampling_job.animation = &state->ozzAnimation;
        sampling_job.ratio = state->ozzAnimationTime / state->ozzAnimation.duration();
        sampling_job.context = &state->ozzSamplingContext;

        ozz::vector<ozz::math::SoaTransform> locals;
        locals.resize(state->ozzSkeleton.num_soa_joints());
        sampling_job.output = make_span(locals);

        if (sampling_job.Run())
        {
            ozz::animation::LocalToModelJob conv_job;
            conv_job.skeleton = &state->ozzSkeleton;
            conv_job.input = make_span(locals);
            conv_job.output = make_span(state->ozzModelMatrices);
            conv_job.Run();
        }
    }

    int winW = 800, winH = 450;
    SDL_GetWindowSizeInPixels(state->window, &winW, &winH);
    glViewport(0, 0, winW, winH);

    float aspect = (float)winW / (float)(winH == 0 ? 1 : winH);

    mat4 proj;
    glm_perspective(glm_rad(45.0f), aspect, 0.1f, 100.0f, proj);

    vec3 eye = { 0.0f, 1.2f, 3.0f };
    vec3 center = { 0.0f, 0.9f, 0.0f };
    vec3 up = { 0.0f, 1.0f, 0.0f };
    mat4 view;
    glm_lookat(eye, center, up, view);

    mat4 model;
    glm_mat4_identity(model);

    mat4 vp;
    glm_mat4_mul(proj, view, vp);

    mat4 mvp;
    glm_mat4_mul(vp, model, mvp);

    glClearColor(0.2f, 0.2f, 0.2f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(state->shaderProgram);
    glUniformMatrix4fv(state->mvpLoc, 1, GL_FALSE, (const GLfloat *)mvp);

    const int activeJointIndices[] = {
        2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
        22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
        40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57,
        58, 59, 60, 61, 62, 63, 64, 65, 66
    };
    const int numActiveJoints = 65;
    std::vector<float> jointMatricesData(numActiveJoints * 16);

    for (int idx = 0; idx < numActiveJoints; ++idx)
    {
        int i = activeJointIndices[idx];
        const ozz::math::Float4x4 &m = state->ozzModelMatrices[i];

        mat4 ozzMat;
        memcpy(ozzMat, &m.cols[0], 16 * sizeof(float));
        mat4 finalJointMat;

        if (idx < (int)state->ybotInverseBindMatrices.size())
        {
            glm_mat4_mul(ozzMat, state->ybotInverseBindMatrices[idx].m, finalJointMat);
        }
        else
        {
            glm_mat4_copy(ozzMat, finalJointMat);
        }

        memcpy(&jointMatricesData[idx * 16], finalJointMat, 16 * sizeof(float));
    }

    glUniformMatrix4fv(state->jointMatricesLoc, numActiveJoints, GL_FALSE, jointMatricesData.data());

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, state->textureID);
    glUniform1i(state->texLoc, 0);

    glBindVertexArray(state->VAO);
    glDrawElements(GL_TRIANGLES, state->indexCount, state->indexType, 0);
    glBindVertexArray(0);

    SDL_GL_SwapWindow(state->window);

    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    if (appstate)
    {
        auto *state = static_cast<AppState *>(appstate);

        if (state->textureID)
            glDeleteTextures(1, &state->textureID);
        if (state->VAO)
            glDeleteVertexArrays(1, &state->VAO);
        if (state->VBO_pos)
            glDeleteBuffers(1, &state->VBO_pos);
        if (state->VBO_uv)
            glDeleteBuffers(1, &state->VBO_uv);
        if (state->VBO_joints)
            glDeleteBuffers(1, &state->VBO_joints);
        if (state->VBO_weights)
            glDeleteBuffers(1, &state->VBO_weights);
        if (state->EBO)
            glDeleteBuffers(1, &state->EBO);
        if (state->shaderProgram)
            glDeleteProgram(state->shaderProgram);

        if (state->glContext)
        {
            SDL_GL_DestroyContext(state->glContext);
        }
        if (state->window)
        {
            SDL_DestroyWindow(state->window);
        }
        delete state;
    }
    SDL_Quit();
}
