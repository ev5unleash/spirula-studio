#pragma once

#include "generated/slang.cuh"

inline __device__ float dot_0(float2  x_0, float2  y_0)
{
    int i_0 = int(0);
    float result_0 = 0.0f;
    for(;;)
    {
        if(i_0 < int(2))
        {
        }
        else
        {
            break;
        }
        float result_1 = result_0 + _slang_vector_get_element(x_0, i_0) * _slang_vector_get_element(y_0, i_0);
        i_0 = i_0 + int(1);
        result_0 = result_1;
    }
    return result_0;
}

inline __device__ float dot_1(float3  x_1, float3  y_1)
{
    int i_1 = int(0);
    float result_2 = 0.0f;
    for(;;)
    {
        if(i_1 < int(3))
        {
        }
        else
        {
            break;
        }
        float result_3 = result_2 + _slang_vector_get_element(x_1, i_1) * _slang_vector_get_element(y_1, i_1);
        i_1 = i_1 + int(1);
        result_2 = result_3;
    }
    return result_2;
}

inline __device__ float length_0(float2  x_2)
{
    return (F32_sqrt((dot_0(x_2, x_2))));
}

inline __device__ float length_1(float3  x_3)
{
    return (F32_sqrt((dot_1(x_3, x_3))));
}

inline __device__ float3  normalize_0(float3  x_4)
{
    return x_4 / make_float3 (length_1(x_4));
}

inline __device__ float3  cross_0(float3  left_0, float3  right_0)
{
    float _S1 = left_0.y;
    float _S2 = right_0.z;
    float _S3 = left_0.z;
    float _S4 = right_0.y;
    float _S5 = right_0.x;
    float _S6 = left_0.x;
    return make_float3 (_S1 * _S2 - _S3 * _S4, _S3 * _S5 - _S6 * _S2, _S6 * _S4 - _S1 * _S5);
}

inline __device__ bool source_project(int model_id_0, FixedArray<float, 16>  params_0, float3  ray_0, float2  * uv_0)
{
    float2  a_0;
    bool _S7;
    float dy_0;
    float dx_0;
    bool _S8;
    bool _S9;
    bool _S10;
    bool _S11;
    bool _S12;
    bool _S13;
    bool _S14;
    for(;;)
    {
        bool _S15 = model_id_0 == int(1000);
        _S8 = _S15;
        if(_S15)
        {
            for(;;)
            {
                *uv_0 = make_float2 (0.0f);
                int base_0 = int(params_0[int(13)]);
                if(base_0 == int(0))
                {
                    float _S16 = ray_0.z;
                    if(_S16 < 9.999999960041972e-13f)
                    {
                        _S7 = false;
                        break;
                    }
                    a_0 = float2 {ray_0.x, ray_0.y} / make_float2 (_S16);
                }
                else
                {
                    float2  _S17 = float2 {ray_0.x, ray_0.y};
                    float r_0 = length_0(_S17);
                    float _S18 = ray_0.z;
                    float theta_0 = (F32_atan2((r_0), (_S18)));
                    if(base_0 == int(1))
                    {
                        if(theta_0 < 0.00100000004749745f)
                        {
                            dx_0 = (1.0f - theta_0 * theta_0 / 3.0f) / _S18;
                        }
                        else
                        {
                            dx_0 = theta_0 / r_0;
                        }
                    }
                    else
                    {
                        if(r_0 < 9.99999997475242708e-07f)
                        {
                            dx_0 = (1.0f - theta_0 * theta_0 / 24.0f) / _S18;
                        }
                        else
                        {
                            dx_0 = 2.0f * (F32_sin((0.5f * theta_0))) / r_0;
                        }
                    }
                    a_0 = _S17 * make_float2 (dx_0);
                }
                float u_0 = a_0.x;
                float v_0 = a_0.y;
                float r2_0 = u_0 * u_0 + v_0 * v_0;
                if((params_0[int(14)]) != 0.0f)
                {
                    float radial_0 = (1.0f + r2_0 * (params_0[int(5)] + r2_0 * (params_0[int(6)] + r2_0 * params_0[int(7)]))) / (1.0f + r2_0 * (params_0[int(8)] + r2_0 * (params_0[int(9)] + r2_0 * params_0[int(10)])));
                    float _S19 = v_0 * radial_0 + 2.0f * params_0[int(12)] * u_0 * v_0 + params_0[int(11)] * (r2_0 + 2.0f * v_0 * v_0);
                    dx_0 = u_0 * radial_0 + 2.0f * params_0[int(11)] * u_0 * v_0 + params_0[int(12)] * (r2_0 + 2.0f * u_0 * u_0);
                    dy_0 = _S19;
                }
                else
                {
                    float radial_1 = 1.0f + r2_0 * (params_0[int(5)] + r2_0 * (params_0[int(6)] + r2_0 * (params_0[int(7)] + r2_0 * params_0[int(8)])));
                    float _S20 = v_0 * radial_1 + 2.0f * params_0[int(10)] * u_0 * v_0 + params_0[int(9)] * (r2_0 + 2.0f * v_0 * v_0) + params_0[int(12)] * r2_0;
                    dx_0 = u_0 * radial_1 + 2.0f * params_0[int(9)] * u_0 * v_0 + params_0[int(10)] * (r2_0 + 2.0f * u_0 * u_0) + params_0[int(11)] * r2_0;
                    dy_0 = _S20;
                }
                *uv_0 = make_float2 (params_0[int(0)] * dx_0 + params_0[int(4)] * dy_0 + params_0[int(2)], params_0[int(1)] * dy_0 + params_0[int(3)]);
                _S7 = true;
                break;
            }
            break;
        }
        if(model_id_0 == int(7))
        {
            for(;;)
            {
                *uv_0 = make_float2 (0.0f);
                float _S21 = ray_0.z;
                if(_S21 < 9.999999960041972e-13f)
                {
                    _S7 = false;
                    break;
                }
                float x_5 = ray_0.x / _S21;
                float y_2 = ray_0.y / _S21;
                float r2_1 = x_5 * x_5 + y_2 * y_2;
                float om2_0 = params_0[int(4)] * params_0[int(4)];
                if(om2_0 < 0.00009999999747379f)
                {
                    dx_0 = om2_0 * r2_1 / 3.0f - om2_0 / 12.0f + 1.0f;
                }
                else
                {
                    if(r2_1 < 0.00009999999747379f)
                    {
                        float t_0 = (F32_tan((params_0[int(4)] * 0.5f)));
                        dx_0 = -2.0f * t_0 * (4.0f * r2_1 * t_0 * t_0 - 3.0f) / (3.0f * params_0[int(4)]);
                    }
                    else
                    {
                        float r_1 = (F32_sqrt((r2_1)));
                        dx_0 = (F32_atan((r_1 * 2.0f * (F32_tan((params_0[int(4)] * 0.5f)))))) / (r_1 * params_0[int(4)]);
                    }
                }
                *uv_0 = make_float2 (params_0[int(0)] * x_5 * dx_0 + params_0[int(2)], params_0[int(1)] * y_2 * dx_0 + params_0[int(3)]);
                _S7 = true;
                break;
            }
            break;
        }
        if(model_id_0 == int(6))
        {
            for(;;)
            {
                *uv_0 = make_float2 (0.0f);
                float _S22 = ray_0.z;
                if(_S22 < 9.999999960041972e-13f)
                {
                    _S7 = false;
                    break;
                }
                float x_6 = ray_0.x / _S22;
                float y_3 = ray_0.y / _S22;
                float r2_2 = x_6 * x_6 + y_3 * y_3;
                float den_0 = 1.0f + r2_2 * (params_0[int(9)] + r2_2 * (params_0[int(10)] + r2_2 * params_0[int(11)]));
                if(!(den_0 > 9.99999997475242708e-07f))
                {
                    _S7 = false;
                    break;
                }
                float radial_2 = (1.0f + r2_2 * (params_0[int(4)] + r2_2 * (params_0[int(5)] + r2_2 * params_0[int(8)]))) / den_0;
                *uv_0 = make_float2 (params_0[int(0)] * (x_6 * radial_2 + 2.0f * params_0[int(6)] * x_6 * y_3 + params_0[int(7)] * (r2_2 + 2.0f * x_6 * x_6)) + params_0[int(2)], params_0[int(1)] * (y_3 * radial_2 + 2.0f * params_0[int(7)] * x_6 * y_3 + params_0[int(6)] * (r2_2 + 2.0f * y_3 * y_3)) + params_0[int(3)]);
                _S7 = true;
                break;
            }
            break;
        }
        if(model_id_0 == int(12))
        {
            for(;;)
            {
                *uv_0 = make_float2 (0.0f);
                float _S23 = ray_0.x;
                float _S24 = ray_0.y;
                float _S25 = ray_0.z;
                float disc_0 = _S25 * _S25 - 4.0f * (_S23 * _S23 + _S24 * _S24) * params_0[int(3)];
                bool _S26 = disc_0 < 0.0f;
                _S9 = _S26;
                if(_S26)
                {
                    break;
                }
                float _S27 = params_0[int(0)] * (2.0f / (_S25 + (F32_sqrt((disc_0)))));
                *uv_0 = make_float2 (_S27 * _S23 + params_0[int(1)], _S27 * _S24 + params_0[int(2)]);
                break;
            }
            _S7 = !_S9;
            break;
        }
        if(model_id_0 == int(13))
        {
            for(;;)
            {
                *uv_0 = make_float2 (0.0f);
                float _S28 = ray_0.x;
                float _S29 = ray_0.y;
                float _S30 = ray_0.z;
                float disc_1 = _S30 * _S30 - 4.0f * (_S28 * _S28 + _S29 * _S29) * params_0[int(4)];
                bool _S31 = disc_1 < 0.0f;
                _S10 = _S31;
                if(_S31)
                {
                    break;
                }
                float r_2 = 2.0f / (_S30 + (F32_sqrt((disc_1))));
                *uv_0 = make_float2 (params_0[int(0)] * r_2 * _S28 + params_0[int(2)], params_0[int(1)] * r_2 * _S29 + params_0[int(3)]);
                break;
            }
            _S7 = !_S10;
            break;
        }
        if(model_id_0 == int(16))
        {
            for(;;)
            {
                *uv_0 = make_float2 (0.0f);
                float _S32 = ray_0.z;
                if(_S32 < 9.999999960041972e-13f)
                {
                    _S7 = false;
                    break;
                }
                float _S33 = ray_0.x;
                float _S34 = ray_0.y;
                float rho2_0 = params_0[int(5)] * (_S33 * _S33 + _S34 * _S34) + _S32 * _S32;
                if(rho2_0 < 0.0f)
                {
                    _S7 = false;
                    break;
                }
                float den_1 = params_0[int(4)] * (F32_sqrt((rho2_0))) + (1.0f - params_0[int(4)]) * _S32;
                if(den_1 < 9.999999960041972e-13f)
                {
                    _S7 = false;
                    break;
                }
                *uv_0 = make_float2 (params_0[int(0)] * _S33 / den_1 + params_0[int(2)], params_0[int(1)] * _S34 / den_1 + params_0[int(3)]);
                _S7 = true;
                break;
            }
            break;
        }
        if(model_id_0 == int(11))
        {
            for(;;)
            {
                *uv_0 = make_float2 (0.0f);
                float _S35 = ray_0.z;
                if(_S35 < 9.999999960041972e-13f)
                {
                    _S7 = false;
                    break;
                }
                float xn_0 = ray_0.x / _S35;
                float yn_0 = ray_0.y / _S35;
                float r_3 = (F32_sqrt((xn_0 * xn_0 + yn_0 * yn_0)));
                if(r_3 > 9.999999960041972e-13f)
                {
                    float th_0 = (F32_atan((r_3)));
                    float vv_0 = yn_0 * (th_0 / r_3);
                    dx_0 = xn_0 * (th_0 / r_3);
                    dy_0 = vv_0;
                }
                else
                {
                    dx_0 = xn_0;
                    dy_0 = yn_0;
                }
                float _S36 = dx_0 * dx_0 + dy_0 * dy_0;
                float pw_0 = _S36 * _S36;
                float pw_1 = pw_0 * _S36;
                float pw_2 = pw_1 * _S36;
                float pw_3 = pw_2 * _S36;
                float rad_0 = 1.0f + params_0[int(4)] * _S36 + params_0[int(5)] * pw_0 + params_0[int(6)] * pw_1 + params_0[int(7)] * pw_2 + params_0[int(8)] * pw_3 + params_0[int(9)] * (pw_3 * _S36);
                float x_7 = rad_0 * dx_0;
                float y_4 = rad_0 * dy_0;
                float x2_0 = x_7 * x_7;
                float y2_0 = y_4 * y_4;
                float xy_0 = x_7 * y_4;
                float r2_3 = x2_0 + y2_0;
                float r4_0 = r2_3 * r2_3;
                *uv_0 = make_float2 (params_0[int(0)] * (x_7 + 2.0f * params_0[int(11)] * xy_0 + params_0[int(10)] * (r2_3 + 2.0f * x2_0) + params_0[int(12)] * r2_3 + params_0[int(13)] * r4_0) + params_0[int(2)], params_0[int(1)] * (y_4 + 2.0f * params_0[int(10)] * xy_0 + params_0[int(11)] * (r2_3 + 2.0f * y2_0) + params_0[int(14)] * r2_3 + params_0[int(15)] * r4_0) + params_0[int(3)]);
                _S7 = true;
                break;
            }
            break;
        }
        *uv_0 = make_float2 (0.0f);
        _S7 = false;
        break;
    }
    if(!_S7)
    {
        return false;
    }
    float2  _S37 = *uv_0;
    for(;;)
    {
        float3  n_0 = normalize_0(ray_0);
        float3  t_1;
        if((F32_abs((n_0.x))) < 0.89999997615814209f)
        {
            t_1 = make_float3 (1.0f, 0.0f, 0.0f);
        }
        else
        {
            t_1 = make_float3 (0.0f, 1.0f, 0.0f);
        }
        float3  e1_0 = normalize_0(cross_0(t_1, n_0));
        float3  e2_0 = cross_0(n_0, e1_0);
        float3  _S38 = n_0 + make_float3 (0.00100000004749745f) * e1_0;
        for(;;)
        {
            if(_S8)
            {
                for(;;)
                {
                    float2  _S39 = make_float2 (0.0f);
                    int base_1 = int(params_0[int(13)]);
                    if(base_1 == int(0))
                    {
                        float _S40 = _S38.z;
                        if(_S40 < 9.999999960041972e-13f)
                        {
                            _S7 = false;
                            a_0 = _S39;
                            break;
                        }
                        a_0 = float2 {_S38.x, _S38.y} / make_float2 (_S40);
                    }
                    else
                    {
                        float2  _S41 = float2 {_S38.x, _S38.y};
                        float r_4 = length_0(_S41);
                        float _S42 = _S38.z;
                        float theta_1 = (F32_atan2((r_4), (_S42)));
                        if(base_1 == int(1))
                        {
                            if(theta_1 < 0.00100000004749745f)
                            {
                                dx_0 = (1.0f - theta_1 * theta_1 / 3.0f) / _S42;
                            }
                            else
                            {
                                dx_0 = theta_1 / r_4;
                            }
                        }
                        else
                        {
                            if(r_4 < 9.99999997475242708e-07f)
                            {
                                dx_0 = (1.0f - theta_1 * theta_1 / 24.0f) / _S42;
                            }
                            else
                            {
                                dx_0 = 2.0f * (F32_sin((0.5f * theta_1))) / r_4;
                            }
                        }
                        a_0 = _S41 * make_float2 (dx_0);
                    }
                    float u_1 = a_0.x;
                    float v_1 = a_0.y;
                    float r2_4 = u_1 * u_1 + v_1 * v_1;
                    if((params_0[int(14)]) != 0.0f)
                    {
                        float radial_3 = (1.0f + r2_4 * (params_0[int(5)] + r2_4 * (params_0[int(6)] + r2_4 * params_0[int(7)]))) / (1.0f + r2_4 * (params_0[int(8)] + r2_4 * (params_0[int(9)] + r2_4 * params_0[int(10)])));
                        float _S43 = v_1 * radial_3 + 2.0f * params_0[int(12)] * u_1 * v_1 + params_0[int(11)] * (r2_4 + 2.0f * v_1 * v_1);
                        dx_0 = u_1 * radial_3 + 2.0f * params_0[int(11)] * u_1 * v_1 + params_0[int(12)] * (r2_4 + 2.0f * u_1 * u_1);
                        dy_0 = _S43;
                    }
                    else
                    {
                        float radial_4 = 1.0f + r2_4 * (params_0[int(5)] + r2_4 * (params_0[int(6)] + r2_4 * (params_0[int(7)] + r2_4 * params_0[int(8)])));
                        float _S44 = v_1 * radial_4 + 2.0f * params_0[int(10)] * u_1 * v_1 + params_0[int(9)] * (r2_4 + 2.0f * v_1 * v_1) + params_0[int(12)] * r2_4;
                        dx_0 = u_1 * radial_4 + 2.0f * params_0[int(9)] * u_1 * v_1 + params_0[int(10)] * (r2_4 + 2.0f * u_1 * u_1) + params_0[int(11)] * r2_4;
                        dy_0 = _S44;
                    }
                    float2  _S45 = make_float2 (params_0[int(0)] * dx_0 + params_0[int(4)] * dy_0 + params_0[int(2)], params_0[int(1)] * dy_0 + params_0[int(3)]);
                    _S7 = true;
                    a_0 = _S45;
                    break;
                }
                break;
            }
            if(model_id_0 == int(7))
            {
                for(;;)
                {
                    float2  _S46 = make_float2 (0.0f);
                    float _S47 = _S38.z;
                    if(_S47 < 9.999999960041972e-13f)
                    {
                        _S7 = false;
                        a_0 = _S46;
                        break;
                    }
                    float x_8 = _S38.x / _S47;
                    float y_5 = _S38.y / _S47;
                    float r2_5 = x_8 * x_8 + y_5 * y_5;
                    float om2_1 = params_0[int(4)] * params_0[int(4)];
                    if(om2_1 < 0.00009999999747379f)
                    {
                        dx_0 = om2_1 * r2_5 / 3.0f - om2_1 / 12.0f + 1.0f;
                    }
                    else
                    {
                        if(r2_5 < 0.00009999999747379f)
                        {
                            float t_2 = (F32_tan((params_0[int(4)] * 0.5f)));
                            dx_0 = -2.0f * t_2 * (4.0f * r2_5 * t_2 * t_2 - 3.0f) / (3.0f * params_0[int(4)]);
                        }
                        else
                        {
                            float r_5 = (F32_sqrt((r2_5)));
                            dx_0 = (F32_atan((r_5 * 2.0f * (F32_tan((params_0[int(4)] * 0.5f)))))) / (r_5 * params_0[int(4)]);
                        }
                    }
                    float2  _S48 = make_float2 (params_0[int(0)] * x_8 * dx_0 + params_0[int(2)], params_0[int(1)] * y_5 * dx_0 + params_0[int(3)]);
                    _S7 = true;
                    a_0 = _S48;
                    break;
                }
                break;
            }
            if(model_id_0 == int(6))
            {
                for(;;)
                {
                    float2  _S49 = make_float2 (0.0f);
                    float _S50 = _S38.z;
                    if(_S50 < 9.999999960041972e-13f)
                    {
                        _S7 = false;
                        a_0 = _S49;
                        break;
                    }
                    float x_9 = _S38.x / _S50;
                    float y_6 = _S38.y / _S50;
                    float r2_6 = x_9 * x_9 + y_6 * y_6;
                    float den_2 = 1.0f + r2_6 * (params_0[int(9)] + r2_6 * (params_0[int(10)] + r2_6 * params_0[int(11)]));
                    if(!(den_2 > 9.99999997475242708e-07f))
                    {
                        _S7 = false;
                        a_0 = _S49;
                        break;
                    }
                    float radial_5 = (1.0f + r2_6 * (params_0[int(4)] + r2_6 * (params_0[int(5)] + r2_6 * params_0[int(8)]))) / den_2;
                    float2  _S51 = make_float2 (params_0[int(0)] * (x_9 * radial_5 + 2.0f * params_0[int(6)] * x_9 * y_6 + params_0[int(7)] * (r2_6 + 2.0f * x_9 * x_9)) + params_0[int(2)], params_0[int(1)] * (y_6 * radial_5 + 2.0f * params_0[int(7)] * x_9 * y_6 + params_0[int(6)] * (r2_6 + 2.0f * y_6 * y_6)) + params_0[int(3)]);
                    _S7 = true;
                    a_0 = _S51;
                    break;
                }
                break;
            }
            if(model_id_0 == int(12))
            {
                for(;;)
                {
                    float2  _S52 = make_float2 (0.0f);
                    float _S53 = _S38.x;
                    float _S54 = _S38.y;
                    float _S55 = _S38.z;
                    float disc_2 = _S55 * _S55 - 4.0f * (_S53 * _S53 + _S54 * _S54) * params_0[int(3)];
                    bool _S56 = disc_2 < 0.0f;
                    _S11 = _S56;
                    if(_S56)
                    {
                        a_0 = _S52;
                        break;
                    }
                    float _S57 = params_0[int(0)] * (2.0f / (_S55 + (F32_sqrt((disc_2)))));
                    a_0 = make_float2 (_S57 * _S53 + params_0[int(1)], _S57 * _S54 + params_0[int(2)]);
                    break;
                }
                _S7 = !_S11;
                break;
            }
            if(model_id_0 == int(13))
            {
                for(;;)
                {
                    float2  _S58 = make_float2 (0.0f);
                    float _S59 = _S38.x;
                    float _S60 = _S38.y;
                    float _S61 = _S38.z;
                    float disc_3 = _S61 * _S61 - 4.0f * (_S59 * _S59 + _S60 * _S60) * params_0[int(4)];
                    bool _S62 = disc_3 < 0.0f;
                    _S12 = _S62;
                    if(_S62)
                    {
                        a_0 = _S58;
                        break;
                    }
                    float r_6 = 2.0f / (_S61 + (F32_sqrt((disc_3))));
                    a_0 = make_float2 (params_0[int(0)] * r_6 * _S59 + params_0[int(2)], params_0[int(1)] * r_6 * _S60 + params_0[int(3)]);
                    break;
                }
                _S7 = !_S12;
                break;
            }
            if(model_id_0 == int(16))
            {
                for(;;)
                {
                    float2  _S63 = make_float2 (0.0f);
                    float _S64 = _S38.z;
                    if(_S64 < 9.999999960041972e-13f)
                    {
                        _S7 = false;
                        a_0 = _S63;
                        break;
                    }
                    float _S65 = _S38.x;
                    float _S66 = _S38.y;
                    float rho2_1 = params_0[int(5)] * (_S65 * _S65 + _S66 * _S66) + _S64 * _S64;
                    if(rho2_1 < 0.0f)
                    {
                        _S7 = false;
                        a_0 = _S63;
                        break;
                    }
                    float den_3 = params_0[int(4)] * (F32_sqrt((rho2_1))) + (1.0f - params_0[int(4)]) * _S64;
                    if(den_3 < 9.999999960041972e-13f)
                    {
                        _S7 = false;
                        a_0 = _S63;
                        break;
                    }
                    float2  _S67 = make_float2 (params_0[int(0)] * _S65 / den_3 + params_0[int(2)], params_0[int(1)] * _S66 / den_3 + params_0[int(3)]);
                    _S7 = true;
                    a_0 = _S67;
                    break;
                }
                break;
            }
            if(model_id_0 == int(11))
            {
                for(;;)
                {
                    float2  _S68 = make_float2 (0.0f);
                    float _S69 = _S38.z;
                    if(_S69 < 9.999999960041972e-13f)
                    {
                        _S7 = false;
                        a_0 = _S68;
                        break;
                    }
                    float xn_1 = _S38.x / _S69;
                    float yn_1 = _S38.y / _S69;
                    float r_7 = (F32_sqrt((xn_1 * xn_1 + yn_1 * yn_1)));
                    if(r_7 > 9.999999960041972e-13f)
                    {
                        float th_1 = (F32_atan((r_7)));
                        float vv_1 = yn_1 * (th_1 / r_7);
                        dx_0 = xn_1 * (th_1 / r_7);
                        dy_0 = vv_1;
                    }
                    else
                    {
                        dx_0 = xn_1;
                        dy_0 = yn_1;
                    }
                    float _S70 = dx_0 * dx_0 + dy_0 * dy_0;
                    float pw_4 = _S70 * _S70;
                    float pw_5 = pw_4 * _S70;
                    float pw_6 = pw_5 * _S70;
                    float pw_7 = pw_6 * _S70;
                    float rad_1 = 1.0f + params_0[int(4)] * _S70 + params_0[int(5)] * pw_4 + params_0[int(6)] * pw_5 + params_0[int(7)] * pw_6 + params_0[int(8)] * pw_7 + params_0[int(9)] * (pw_7 * _S70);
                    float x_10 = rad_1 * dx_0;
                    float y_7 = rad_1 * dy_0;
                    float x2_1 = x_10 * x_10;
                    float y2_1 = y_7 * y_7;
                    float xy_1 = x_10 * y_7;
                    float r2_7 = x2_1 + y2_1;
                    float r4_1 = r2_7 * r2_7;
                    float2  _S71 = make_float2 (params_0[int(0)] * (x_10 + 2.0f * params_0[int(11)] * xy_1 + params_0[int(10)] * (r2_7 + 2.0f * x2_1) + params_0[int(12)] * r2_7 + params_0[int(13)] * r4_1) + params_0[int(2)], params_0[int(1)] * (y_7 + 2.0f * params_0[int(10)] * xy_1 + params_0[int(11)] * (r2_7 + 2.0f * y2_1) + params_0[int(14)] * r2_7 + params_0[int(15)] * r4_1) + params_0[int(3)]);
                    _S7 = true;
                    a_0 = _S71;
                    break;
                }
                break;
            }
            float2  _S72 = make_float2 (0.0f);
            _S7 = false;
            a_0 = _S72;
            break;
        }
        if(!_S7)
        {
            _S7 = false;
            break;
        }
        float2  b_0;
        float3  _S73 = n_0 + make_float3 (0.00100000004749745f) * e2_0;
        for(;;)
        {
            if(_S8)
            {
                for(;;)
                {
                    float2  _S74 = make_float2 (0.0f);
                    int base_2 = int(params_0[int(13)]);
                    if(base_2 == int(0))
                    {
                        float _S75 = _S73.z;
                        if(_S75 < 9.999999960041972e-13f)
                        {
                            _S7 = false;
                            b_0 = _S74;
                            break;
                        }
                        b_0 = float2 {_S73.x, _S73.y} / make_float2 (_S75);
                    }
                    else
                    {
                        float2  _S76 = float2 {_S73.x, _S73.y};
                        float r_8 = length_0(_S76);
                        float _S77 = _S73.z;
                        float theta_2 = (F32_atan2((r_8), (_S77)));
                        if(base_2 == int(1))
                        {
                            if(theta_2 < 0.00100000004749745f)
                            {
                                dx_0 = (1.0f - theta_2 * theta_2 / 3.0f) / _S77;
                            }
                            else
                            {
                                dx_0 = theta_2 / r_8;
                            }
                        }
                        else
                        {
                            if(r_8 < 9.99999997475242708e-07f)
                            {
                                dx_0 = (1.0f - theta_2 * theta_2 / 24.0f) / _S77;
                            }
                            else
                            {
                                dx_0 = 2.0f * (F32_sin((0.5f * theta_2))) / r_8;
                            }
                        }
                        b_0 = _S76 * make_float2 (dx_0);
                    }
                    float u_2 = b_0.x;
                    float v_2 = b_0.y;
                    float r2_8 = u_2 * u_2 + v_2 * v_2;
                    if((params_0[int(14)]) != 0.0f)
                    {
                        float radial_6 = (1.0f + r2_8 * (params_0[int(5)] + r2_8 * (params_0[int(6)] + r2_8 * params_0[int(7)]))) / (1.0f + r2_8 * (params_0[int(8)] + r2_8 * (params_0[int(9)] + r2_8 * params_0[int(10)])));
                        float _S78 = v_2 * radial_6 + 2.0f * params_0[int(12)] * u_2 * v_2 + params_0[int(11)] * (r2_8 + 2.0f * v_2 * v_2);
                        dx_0 = u_2 * radial_6 + 2.0f * params_0[int(11)] * u_2 * v_2 + params_0[int(12)] * (r2_8 + 2.0f * u_2 * u_2);
                        dy_0 = _S78;
                    }
                    else
                    {
                        float radial_7 = 1.0f + r2_8 * (params_0[int(5)] + r2_8 * (params_0[int(6)] + r2_8 * (params_0[int(7)] + r2_8 * params_0[int(8)])));
                        float _S79 = v_2 * radial_7 + 2.0f * params_0[int(10)] * u_2 * v_2 + params_0[int(9)] * (r2_8 + 2.0f * v_2 * v_2) + params_0[int(12)] * r2_8;
                        dx_0 = u_2 * radial_7 + 2.0f * params_0[int(9)] * u_2 * v_2 + params_0[int(10)] * (r2_8 + 2.0f * u_2 * u_2) + params_0[int(11)] * r2_8;
                        dy_0 = _S79;
                    }
                    float2  _S80 = make_float2 (params_0[int(0)] * dx_0 + params_0[int(4)] * dy_0 + params_0[int(2)], params_0[int(1)] * dy_0 + params_0[int(3)]);
                    _S7 = true;
                    b_0 = _S80;
                    break;
                }
                break;
            }
            if(model_id_0 == int(7))
            {
                for(;;)
                {
                    float2  _S81 = make_float2 (0.0f);
                    float _S82 = _S73.z;
                    if(_S82 < 9.999999960041972e-13f)
                    {
                        _S7 = false;
                        b_0 = _S81;
                        break;
                    }
                    float x_11 = _S73.x / _S82;
                    float y_8 = _S73.y / _S82;
                    float r2_9 = x_11 * x_11 + y_8 * y_8;
                    float om2_2 = params_0[int(4)] * params_0[int(4)];
                    if(om2_2 < 0.00009999999747379f)
                    {
                        dx_0 = om2_2 * r2_9 / 3.0f - om2_2 / 12.0f + 1.0f;
                    }
                    else
                    {
                        if(r2_9 < 0.00009999999747379f)
                        {
                            float t_3 = (F32_tan((params_0[int(4)] * 0.5f)));
                            dx_0 = -2.0f * t_3 * (4.0f * r2_9 * t_3 * t_3 - 3.0f) / (3.0f * params_0[int(4)]);
                        }
                        else
                        {
                            float r_9 = (F32_sqrt((r2_9)));
                            dx_0 = (F32_atan((r_9 * 2.0f * (F32_tan((params_0[int(4)] * 0.5f)))))) / (r_9 * params_0[int(4)]);
                        }
                    }
                    float2  _S83 = make_float2 (params_0[int(0)] * x_11 * dx_0 + params_0[int(2)], params_0[int(1)] * y_8 * dx_0 + params_0[int(3)]);
                    _S7 = true;
                    b_0 = _S83;
                    break;
                }
                break;
            }
            if(model_id_0 == int(6))
            {
                for(;;)
                {
                    float2  _S84 = make_float2 (0.0f);
                    float _S85 = _S73.z;
                    if(_S85 < 9.999999960041972e-13f)
                    {
                        _S7 = false;
                        b_0 = _S84;
                        break;
                    }
                    float x_12 = _S73.x / _S85;
                    float y_9 = _S73.y / _S85;
                    float r2_10 = x_12 * x_12 + y_9 * y_9;
                    float den_4 = 1.0f + r2_10 * (params_0[int(9)] + r2_10 * (params_0[int(10)] + r2_10 * params_0[int(11)]));
                    if(!(den_4 > 9.99999997475242708e-07f))
                    {
                        _S7 = false;
                        b_0 = _S84;
                        break;
                    }
                    float radial_8 = (1.0f + r2_10 * (params_0[int(4)] + r2_10 * (params_0[int(5)] + r2_10 * params_0[int(8)]))) / den_4;
                    float2  _S86 = make_float2 (params_0[int(0)] * (x_12 * radial_8 + 2.0f * params_0[int(6)] * x_12 * y_9 + params_0[int(7)] * (r2_10 + 2.0f * x_12 * x_12)) + params_0[int(2)], params_0[int(1)] * (y_9 * radial_8 + 2.0f * params_0[int(7)] * x_12 * y_9 + params_0[int(6)] * (r2_10 + 2.0f * y_9 * y_9)) + params_0[int(3)]);
                    _S7 = true;
                    b_0 = _S86;
                    break;
                }
                break;
            }
            if(model_id_0 == int(12))
            {
                for(;;)
                {
                    float2  _S87 = make_float2 (0.0f);
                    float _S88 = _S73.x;
                    float _S89 = _S73.y;
                    float _S90 = _S73.z;
                    float disc_4 = _S90 * _S90 - 4.0f * (_S88 * _S88 + _S89 * _S89) * params_0[int(3)];
                    bool _S91 = disc_4 < 0.0f;
                    _S13 = _S91;
                    if(_S91)
                    {
                        b_0 = _S87;
                        break;
                    }
                    float _S92 = params_0[int(0)] * (2.0f / (_S90 + (F32_sqrt((disc_4)))));
                    b_0 = make_float2 (_S92 * _S88 + params_0[int(1)], _S92 * _S89 + params_0[int(2)]);
                    break;
                }
                _S7 = !_S13;
                break;
            }
            if(model_id_0 == int(13))
            {
                for(;;)
                {
                    float2  _S93 = make_float2 (0.0f);
                    float _S94 = _S73.x;
                    float _S95 = _S73.y;
                    float _S96 = _S73.z;
                    float disc_5 = _S96 * _S96 - 4.0f * (_S94 * _S94 + _S95 * _S95) * params_0[int(4)];
                    bool _S97 = disc_5 < 0.0f;
                    _S14 = _S97;
                    if(_S97)
                    {
                        b_0 = _S93;
                        break;
                    }
                    float r_10 = 2.0f / (_S96 + (F32_sqrt((disc_5))));
                    b_0 = make_float2 (params_0[int(0)] * r_10 * _S94 + params_0[int(2)], params_0[int(1)] * r_10 * _S95 + params_0[int(3)]);
                    break;
                }
                _S7 = !_S14;
                break;
            }
            if(model_id_0 == int(16))
            {
                for(;;)
                {
                    float2  _S98 = make_float2 (0.0f);
                    float _S99 = _S73.z;
                    if(_S99 < 9.999999960041972e-13f)
                    {
                        _S7 = false;
                        b_0 = _S98;
                        break;
                    }
                    float _S100 = _S73.x;
                    float _S101 = _S73.y;
                    float rho2_2 = params_0[int(5)] * (_S100 * _S100 + _S101 * _S101) + _S99 * _S99;
                    if(rho2_2 < 0.0f)
                    {
                        _S7 = false;
                        b_0 = _S98;
                        break;
                    }
                    float den_5 = params_0[int(4)] * (F32_sqrt((rho2_2))) + (1.0f - params_0[int(4)]) * _S99;
                    if(den_5 < 9.999999960041972e-13f)
                    {
                        _S7 = false;
                        b_0 = _S98;
                        break;
                    }
                    float2  _S102 = make_float2 (params_0[int(0)] * _S100 / den_5 + params_0[int(2)], params_0[int(1)] * _S101 / den_5 + params_0[int(3)]);
                    _S7 = true;
                    b_0 = _S102;
                    break;
                }
                break;
            }
            if(model_id_0 == int(11))
            {
                for(;;)
                {
                    float2  _S103 = make_float2 (0.0f);
                    float _S104 = _S73.z;
                    if(_S104 < 9.999999960041972e-13f)
                    {
                        _S7 = false;
                        b_0 = _S103;
                        break;
                    }
                    float xn_2 = _S73.x / _S104;
                    float yn_2 = _S73.y / _S104;
                    float r_11 = (F32_sqrt((xn_2 * xn_2 + yn_2 * yn_2)));
                    if(r_11 > 9.999999960041972e-13f)
                    {
                        float th_2 = (F32_atan((r_11)));
                        float vv_2 = yn_2 * (th_2 / r_11);
                        dx_0 = xn_2 * (th_2 / r_11);
                        dy_0 = vv_2;
                    }
                    else
                    {
                        dx_0 = xn_2;
                        dy_0 = yn_2;
                    }
                    float _S105 = dx_0 * dx_0 + dy_0 * dy_0;
                    float pw_8 = _S105 * _S105;
                    float pw_9 = pw_8 * _S105;
                    float pw_10 = pw_9 * _S105;
                    float pw_11 = pw_10 * _S105;
                    float rad_2 = 1.0f + params_0[int(4)] * _S105 + params_0[int(5)] * pw_8 + params_0[int(6)] * pw_9 + params_0[int(7)] * pw_10 + params_0[int(8)] * pw_11 + params_0[int(9)] * (pw_11 * _S105);
                    float x_13 = rad_2 * dx_0;
                    float y_10 = rad_2 * dy_0;
                    float x2_2 = x_13 * x_13;
                    float y2_2 = y_10 * y_10;
                    float xy_2 = x_13 * y_10;
                    float r2_11 = x2_2 + y2_2;
                    float r4_2 = r2_11 * r2_11;
                    float2  _S106 = make_float2 (params_0[int(0)] * (x_13 + 2.0f * params_0[int(11)] * xy_2 + params_0[int(10)] * (r2_11 + 2.0f * x2_2) + params_0[int(12)] * r2_11 + params_0[int(13)] * r4_2) + params_0[int(2)], params_0[int(1)] * (y_10 + 2.0f * params_0[int(10)] * xy_2 + params_0[int(11)] * (r2_11 + 2.0f * y2_2) + params_0[int(14)] * r2_11 + params_0[int(15)] * r4_2) + params_0[int(3)]);
                    _S7 = true;
                    b_0 = _S106;
                    break;
                }
                break;
            }
            float2  _S107 = make_float2 (0.0f);
            _S7 = false;
            b_0 = _S107;
            break;
        }
        if(!_S7)
        {
            _S7 = false;
            break;
        }
        float2  j1_0 = a_0 - _S37;
        float2  j2_0 = b_0 - _S37;
        _S7 = (j1_0.x * j2_0.y - j1_0.y * j2_0.x) > 0.0f;
        break;
    }
    return _S7;
}

