# Findie Changelog

### Full list
https://github.com/findie/FFmpeg/commits/findie?author=legraphista

#### 2018 07 18

- vf_framechange
    - [ef56b71](https://github.com/findie/FFmpeg/commit/ef56b71961800b1df2b7e73205481ac01f8d92f2) added filter

#### 2018 03 01

- vf_drawbox
    - [6b6097a](https://github.com/findie/FFmpeg/commit/6b6097a04660ea7d433415e23e8a3acf30d49a42) added color expressions 
        - `color_alpha_expr`: manipulate the color's alpha channel
        - `color_red_expr`  : manipulate the color's red channel
        - `color_green_expr`: manipulate the color's green channel
        - `color_blue_expr` : manipulate the color's blue channel
        - `color_y_expr`    : manipulate the color's Y channel
        - `color_u_expr`    : manipulate the color's U channel
        - `color_v_expr`    : manipulate the color's V channel

#### 2018 02 27

- vf_zoom
    - [09e61dc](https://github.com/findie/FFmpeg/commit/09e61dc1f9091b0dee476a8d5c0124532895e066) added filter
        - `zoom`/`z`: [`1`] expression to set the zooming of the frame
        - `fillcolor`: [`black@0`] set the background color (used when zoom < 1 or frame has transparency)
        - `interpolation`: [`fast_bilinear`] interpolation algorithm
            - `fast_bilinear` 
            - `bilinear` 
            - `bicubic` 
            - `x` 
            - `point` 
            - `area` 
            - `bicublin` 
            - `gauss` 
            - `sinc` 
            - `lanczos` 
            - `spline` 

#### 2018 02 15 
    
- vf_framechange
    - [a480486](https://github.com/findie/FFmpeg/commit/a480486c79d0e7f24ff7e343a912a48bf76628ab) added filter
        - `threshold`: [`10`] the minimum amount of pixel change before a change is registered
        - `mode`: [`absolute`] the mode in which the pixels get counted
            - `absolute`: a changed pixel counts as 1
            - `percentage`: a changed pixel counts as the absolute difference between the sources, divided by 255
        - `show`: [`0`] render video frames with changes    

#### 2018 02 13

- vf_drawbox
    - [a8cd882](https://github.com/findie/FFmpeg/commit/a8cd88258b2691dc36403140ef4b966dd515ee80) breaking change: `t` will stand for time in expressions, `thickness` for thickness

#### 2018 01 26

- vf_drawtext
    - [4b728a1](https://github.com/findie/FFmpeg/commit/4b728a1673efdec99d47a3010d944fed4a5955b3) update `clip_*` to be expressions and to be accessed from other expressions

#### 2018 01 18

- vf_drawtext
    - [e7f31da](https://github.com/findie/FFmpeg/commit/e7f31da455b6e905efb882eac2ccd44b6975b3a7) add `clip_enable` flag to disabled clipping of text

#### 2018 01 17

- vf_drawtext
    - [c28495c](https://github.com/findie/FFmpeg/commit/c28495c55c6fb75a8ad5edc9e92b40761f49c791) add `clip_(top|left|right|bottom)` to clip text 
    - [fd38d34](https://github.com/findie/FFmpeg/commit/fd38d347b2545970c2db526491f6d5090ab8385e) add `offset(x|y)` to offset text

#### 2018 01 16

- vf_drawtext
    - [9f921d7](https://github.com/findie/FFmpeg/commit/9f921d7f28dc835c0966afecb29873458440aeca) add `word_spacing` that overrides `\s` and `\t` 

#### 2018 01 13

- vf_drawbox
    - [1a534fa](https://github.com/findie/FFmpeg/commit/1a534fa7346858e7996877121c3ffa5e91c83150) fix x/y position being behind by one frame

#### 2018 01 12

- vf_drawbox
    - [a85563f](https://github.com/findie/FFmpeg/commit/a85563f5c3cad453228b7f0a0dabccacb768b877) add `time` as a variable in expressions
    - [a85563f](https://github.com/findie/FFmpeg/commit/a85563f5c3cad453228b7f0a0dabccacb768b877) enabled expression parsing every frame 
