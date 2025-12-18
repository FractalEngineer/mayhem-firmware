#!/usr/bin/env python3
"""
Quick UI Layout Test for Spectrum Analyzer App
Automatically parses spectrum_analyzer_app.hpp to visualize widget positions
"""

import tkinter as tk
import re
import os
from pathlib import Path

# PortaPack screen dimensions
SCREEN_WIDTH = 240
SCREEN_HEIGHT = 320
SCALE = 2

# UI_POS_Y macro: UI_POS_Y(linenum) = linenum * 16
def ui_pos_y(line_num):
    return line_num * 16

class UIParser:
    """Parses the spectrum analyzer header file to extract UI information"""
    
    def __init__(self, header_file):
        self.header_file = header_file
        self.content = self._read_file()
        self.constants = {}
        self.widgets = []
        self.labels = []
        
    def _read_file(self):
        """Read the header file"""
        if not os.path.exists(self.header_file):
            # Try relative path from script location
            script_dir = Path(__file__).parent
            self.header_file = script_dir / self.header_file
        with open(self.header_file, 'r', encoding='utf-8') as f:
            return f.read()
    
    def _eval_expression(self, expr):
        """Evaluate a C++ expression like '4 * 16' or 'UI_POS_Y(0)'"""
        expr = expr.strip()
        
        # Handle UI_POS_Y macro
        ui_pos_match = re.search(r'UI_POS_Y\((\d+)\)', expr)
        if ui_pos_match:
            line_num = int(ui_pos_match.group(1))
            return ui_pos_y(line_num)
        
        # Handle simple arithmetic expressions
        try:
            # Replace * with * for multiplication
            expr = expr.replace('*', '*')
            # Evaluate safely (only allow numbers, *, +, -, /, spaces, parentheses)
            if re.match(r'^[\d\s\*\+\-\(\)\/]+$', expr):
                return eval(expr)
        except:
            pass
        
        return 0
    
    def parse_constants(self):
        """Extract constants like header_height, fft_height"""
        # Match: static constexpr ui::Dim header_height = 4 * 16;
        pattern = r'static\s+constexpr\s+[^=]+=\s*([^;]+);'
        for match in re.finditer(pattern, self.content):
            name_match = re.search(r'(\w+)\s*=', match.group(0))
            if name_match:
                name = name_match.group(1)
                value_expr = match.group(1).strip()
                self.constants[name] = self._eval_expression(value_expr)
    
    def parse_labels(self):
        """Parse Labels widget with multiple label entries"""
        # Match Labels widget with multiple entries - need to handle nested braces
        # Look for Labels widget_name{ ... };
        labels_pattern = r'Labels\s+\w+\s*\{'
        match = re.search(labels_pattern, self.content)
        if match:
            # Find the matching closing brace for the Labels widget
            start_pos = match.end()
            brace_count = 1
            pos = start_pos
            while pos < len(self.content) and brace_count > 0:
                if self.content[pos] == '{':
                    brace_count += 1
                elif self.content[pos] == '}':
                    brace_count -= 1
                pos += 1
            
            if brace_count == 0:
                labels_content = self.content[start_pos:pos-1]
                # Match each label entry: {{x * 8, UI_POS_Y(n)}, "TEXT", ...}
                # Handle both single and multi-line entries
                label_pattern = r'\{\{([^}]+)\},\s*"([^"]+)"'
                for label_match in re.finditer(label_pattern, labels_content):
                    coords = label_match.group(1)
                    text = label_match.group(2)
                    
                    # Parse coordinates: "0 * 8, UI_POS_Y(0)"
                    coord_parts = [p.strip() for p in coords.split(',')]
                    if len(coord_parts) >= 2:
                        x = self._eval_expression(coord_parts[0])
                        y = self._eval_expression(coord_parts[1])
                        self.labels.append({
                            'x': x,
                            'y': y,
                            'text': text,
                            'type': 'Label'
                        })
    
    def parse_widgets(self):
        """Parse various widget types"""
        # Widget patterns: Type name{ {x, y, ...}, ... };
        widget_patterns = [
            (r'RSSI\s+\w+\s*\{\s*\{([^}]+)\}\s*\}', 'RSSI', 'cyan', None),
            (r'Channel\s+\w+\s*\{\s*\{([^}]+)\}\s*\}', 'Channel', 'cyan', None),
            (r'RxFrequencyField\s+(\w+)\s*\{\s*\{([^}]+)\}', 'RxFrequencyField', 'darkgreen', None),
            (r'LNAGainField\s+(\w+)\s*\{\s*\{([^}]+)\}', 'LNAGainField', 'darkorange', None),
            (r'VGAGainField\s+(\w+)\s*\{\s*\{([^}]+)\}', 'VGAGainField', 'darkorange', None),
            (r'RFAmpField\s+(\w+)\s*\{\s*\{([^}]+)\}', 'RFAmpField', 'darkorange', None),
            (r'OptionsField\s+(\w+)\s*\{\s*\{([^}]+)\}', 'OptionsField', 'darkmagenta', None),
            (r'Text\s+(\w+)\s*\{\s*\{([^}]+)\},\s*"([^"]+)"', 'Text', 'darkcyan', 3),
        ]
        
        for pattern, widget_type, color, text_group in widget_patterns:
            for match in re.finditer(pattern, self.content):
                # Extract name and coordinates based on pattern
                if widget_type in ['RSSI', 'Channel']:
                    name = widget_type
                    coords_str = match.group(1)
                elif widget_type == 'Text':
                    name = match.group(1)
                    coords_str = match.group(2)
                    text = match.group(3)
                else:
                    name = match.group(1)
                    coords_str = match.group(2)
                    text = None
                
                # Parse coordinates
                coord_parts = [p.strip() for p in coords_str.split(',')]
                if len(coord_parts) >= 2:
                    x = self._eval_expression(coord_parts[0])
                    y = self._eval_expression(coord_parts[1])
                    
                    # Default sizes for different widget types
                    width = 4 * 8  # Default width
                    height = 16    # Default height
                    
                    if len(coord_parts) >= 4:
                        width = self._eval_expression(coord_parts[2])
                        height = self._eval_expression(coord_parts[3])
                    
                    # Special handling for OptionsField - check for width parameter
                    if widget_type == 'OptionsField':
                        # Look for width parameter after coordinates: OptionsField name{ {x, y}, width, ...
                        options_match = re.search(
                            r'OptionsField\s+' + re.escape(name) + r'\s*\{[^}]+\},\s*(\d+)',
                            self.content[max(0, match.start()-100):match.end()+100]
                        )
                        if options_match:
                            width = int(options_match.group(1)) * 8
                    
                    # Create widget entry
                    widget_entry = {
                        'name': name,
                        'type': widget_type,
                        'x': x,
                        'y': y,
                        'width': width,
                        'height': height,
                        'color': color,
                    }
                    
                    if widget_type == 'Text':
                        widget_entry['text'] = text
                    
                    self.widgets.append(widget_entry)
    
    def parse_views(self):
        """Parse view definitions like fft_view and waterfall"""
        # Parse fft_view: SpectrumFFTView fft_view{{0, header_height, screen_width, fft_height}};
        fft_match = re.search(r'fft_view\{\{([^}]+)\}\}', self.content)
        if fft_match:
            coords = [self._eval_expression(c.strip()) for c in fft_match.group(1).split(',')]
            if len(coords) >= 4:
                # Use header_height constant if available
                if 'header_height' in self.constants:
                    coords[1] = self.constants['header_height']
                if 'fft_height' in self.constants:
                    coords[3] = self.constants['fft_height']
                
                self.widgets.append({
                    'name': 'FFT View',
                    'type': 'View',
                    'x': coords[0],
                    'y': coords[1],
                    'width': coords[2],
                    'height': coords[3],
                    'color': 'darkred',
                    'text': 'Spectrum Waveform'
                })
        
        # Waterfall starts after header + fft
        if 'header_height' in self.constants and 'fft_height' in self.constants:
            waterfall_y = self.constants['header_height'] + self.constants['fft_height']
            waterfall_height = SCREEN_HEIGHT - waterfall_y
            self.widgets.append({
                'name': 'Waterfall',
                'type': 'View',
                'x': 0,
                'y': waterfall_y,
                'width': SCREEN_WIDTH,
                'height': waterfall_height,
                'color': 'darkgreen',
                'text': 'Waterfall Display'
            })
    
    def parse(self):
        """Parse all UI information from the header file"""
        self.parse_constants()
        self.parse_labels()
        self.parse_widgets()
        self.parse_views()
        return self

class SpectrumAnalyzerUITest:
    def __init__(self, root, header_file='spectrum_analyzer_app.hpp'):
        self.root = root
        self.root.title("Spectrum Analyzer UI Layout Test")
        
        # Parse UI from header file
        try:
            parser = UIParser(header_file)
            parser.parse()
            self.constants = parser.constants
            self.widgets = parser.widgets
            self.labels = parser.labels
            self.header_file = parser.header_file
            
            # Debug output
            print(f"Parsed {len(self.labels)} labels: {[l['text'] for l in self.labels]}")
            print(f"Parsed {len(self.widgets)} widgets")
        except Exception as e:
            # Fallback to defaults if parsing fails
            import traceback
            print(f"Warning: Failed to parse header file: {e}")
            traceback.print_exc()
            print("Using default values...")
            self.constants = {'header_height': 64, 'fft_height': 32}
            self.widgets = []
            self.labels = []
            self.header_file = header_file
        
        # Create canvas
        canvas_width = SCREEN_WIDTH * SCALE
        canvas_height = SCREEN_HEIGHT * SCALE
        self.canvas = tk.Canvas(
            root, 
            width=canvas_width, 
            height=canvas_height, 
            bg='black',
            highlightthickness=0
        )
        self.canvas.pack(padx=10, pady=10)
        
        # Draw UI layout
        self.draw_layout()
        
        # Add info label
        self.add_info_panel()
        
    def add_info_panel(self):
        """Add information panel showing parsed data"""
        header_height = self.constants.get('header_height', 64)
        fft_height = self.constants.get('fft_height', 32)
        waterfall_y = header_height + fft_height
        waterfall_height = SCREEN_HEIGHT - waterfall_y
        
        widget_count = len(self.widgets) + len(self.labels)
        source_name = os.path.basename(getattr(self, 'header_file', 'spectrum_analyzer_app.hpp'))
        info_text = (
            f"Spectrum Analyzer UI Layout Test (Auto-parsed from {source_name})\n"
            f"Screen: {SCREEN_WIDTH}x{SCREEN_HEIGHT}px | "
            f"Header: {header_height}px | FFT: {fft_height}px | Waterfall: {waterfall_height}px\n"
            f"Found {len(self.widgets)} widgets, {len(self.labels)} labels"
        )
        info_label = tk.Label(
            self.root, 
            text=info_text, 
            justify=tk.LEFT,
            font=('Courier', 8),
            bg='lightgray',
            padx=5,
            pady=5
        )
        info_label.pack(pady=5, fill=tk.X)
        
    def draw_layout(self):
        """Draw the UI layout from parsed data"""
        header_height = self.constants.get('header_height', 64)
        
        # Draw header section first (background)
        self.draw_rect(0, 0, SCREEN_WIDTH, header_height, 'darkblue', 'Header Section')
        
        # Draw widgets first (so labels appear on top)
        for widget in self.widgets:
            if widget['type'] == 'View':
                # Draw view with text
                self.draw_rect(widget['x'], widget['y'], widget['width'], widget['height'], 
                             widget['color'], widget.get('text', widget['name']))
            else:
                # Draw widget
                label = widget.get('text') or widget['name']
                self.draw_rect(widget['x'], widget['y'], widget['width'], widget['height'], 
                               widget['color'], label)
        
        # Draw labels last (on top) with visible backgrounds
        for label in self.labels:
            self.draw_label(label['x'], label['y'], label['text'])
        
        # Draw grid lines for reference
        self.draw_grid()
        
        # Add coordinate labels at corners
        self.canvas.create_text(5, 5, text="(0,0)", fill='gray60', font=('Courier', 6), anchor='nw')
        self.canvas.create_text(SCREEN_WIDTH * SCALE - 5, 5, text=f"({SCREEN_WIDTH},0)", 
                               fill='gray60', font=('Courier', 6), anchor='ne')
        self.canvas.create_text(5, SCREEN_HEIGHT * SCALE - 5, text=f"(0,{SCREEN_HEIGHT})", 
                               fill='gray60', font=('Courier', 6), anchor='sw')
        self.canvas.create_text(SCREEN_WIDTH * SCALE - 5, SCREEN_HEIGHT * SCALE - 5, 
                               text=f"({SCREEN_WIDTH},{SCREEN_HEIGHT})", fill='gray60', font=('Courier', 6), anchor='se')
        
    def draw_rect(self, x, y, width, height, color, label):
        """Draw a rectangle representing a widget"""
        x1 = x * SCALE
        y1 = y * SCALE
        x2 = (x + width) * SCALE
        y2 = (y + height) * SCALE
        
        # Draw rectangle with outline and light fill
        fill_color = self.lighten_color(color)
        self.canvas.create_rectangle(x1, y1, x2, y2, outline=color, fill=fill_color, width=2)
        
        # Add label if there's space
        if width > 20 and height > 10:
            # Split label if it's too long
            if len(label) > 15 and width < 80:
                # Try to split on spaces
                words = label.split()
                if len(words) > 1:
                    mid = len(words) // 2
                    label = ' '.join(words[:mid]) + '\n' + ' '.join(words[mid:])
            
            self.canvas.create_text(
                (x1 + x2) // 2, 
                (y1 + y2) // 2, 
                text=label, 
                fill='white', 
                font=('Courier', 7, 'bold'),
                justify='center'
            )
    
    def lighten_color(self, color_name):
        """Convert color name to a lighter version for fill"""
        color_map = {
            'darkblue': '#1a1a4d',
            'darkgreen': '#1a4d1a',
            'darkorange': '#4d3a1a',
            'darkmagenta': '#4d1a4d',
            'darkcyan': '#1a4d4d',
            'darkred': '#4d1a1a',
            'cyan': '#1a4d4d',
        }
        return color_map.get(color_name, '#333333')
    
    def draw_label(self, x, y, text):
        """Draw a label with visible background"""
        # Estimate label width based on text length (add some padding)
        text_width = max(len(text) * 8, 3 * 8)  # At least 3 characters wide
        text_height = 16
        
        x1 = x * SCALE
        y1 = y * SCALE
        x2 = (x + text_width) * SCALE
        y2 = (y + text_height) * SCALE
        
        # Draw background rectangle for label (darker background for contrast)
        self.canvas.create_rectangle(
            x1, y1, x2, y2,
            outline='yellow',
            fill='#4d4d00',  # Darker yellow background for better contrast
            width=2,
            tags='label_bg'
        )
        
        # Draw label text (bright yellow, bold)
        self.canvas.create_text(
            (x1 + x2) // 2,
            (y1 + y2) // 2,
            text=text,
            fill='#ffff00',  # Bright yellow
            font=('Courier', 9, 'bold'),
            anchor='center',
            tags='label_text'
        )
    
    def draw_text(self, x, y, text, color, size):
        """Draw text at position"""
        self.canvas.create_text(
            x * SCALE, 
            y * SCALE, 
            text=text, 
            fill=color, 
            font=('Courier', size),
            anchor='nw'
        )
    
    def draw_grid(self):
        """Draw a subtle grid for reference"""
        for x in range(0, SCREEN_WIDTH, 8):
            self.canvas.create_line(
                x * SCALE, 0, 
                x * SCALE, SCREEN_HEIGHT * SCALE,
                fill='gray20', 
                width=1
            )
        for y in range(0, SCREEN_HEIGHT, 16):
            self.canvas.create_line(
                0, y * SCALE,
                SCREEN_WIDTH * SCALE, y * SCALE,
                fill='gray20',
                width=1
            )

def main():
    import sys
    
    # Get header file path (default to same directory as script)
    if len(sys.argv) > 1:
        header_file = sys.argv[1]
    else:
        script_dir = Path(__file__).parent
        header_file = script_dir / 'spectrum_analyzer_app.hpp'
    
    root = tk.Tk()
    app = SpectrumAnalyzerUITest(root, str(header_file))
    root.mainloop()

if __name__ == "__main__":
    main()
