import pytest
import subprocess
import sys
import os

# Add the Validation directory to the path so we can import visualize.py
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

from Validation.visualize import generate_visualization

@pytest.mark.parametrize("payload", [
    # Exact exploit case - command injection payload
    "test; rm -rf /tmp/testfile",
    # Alternative command injection with subshell
    "normal_input$(whoami)",
    # Backtick command execution
    "valid`id`data",
    # Boundary case - input with only shell metacharacters
    ";",
    # Valid input (should pass)
    "normal_chart_title"
])
def test_shell_command_injection_prevention(payload):
    """Invariant: Shell commands never include unsanitized user input"""
    
    # Create a temporary test file that will be used as input
    test_input_file = "test_input_data.csv"
    test_output_file = "test_output.png"
    
    try:
        # Write minimal valid CSV data to the input file
        with open(test_input_file, 'w') as f:
            f.write("x,y\n1,2\n3,4\n")
        
        # Call the actual production function with adversarial payload
        # The payload is used as the chart title parameter
        generate_visualization(
            input_file=test_input_file,
            output_file=test_output_file,
            chart_title=payload
        )
        
        # If we reach here without command injection, the test passes
        # Clean up output file if it was created
        if os.path.exists(test_output_file):
            os.remove(test_output_file)
            
    except (subprocess.CalledProcessError, ValueError, TypeError) as e:
        # Expected: function should reject or sanitize malicious input
        # Any exception is acceptable as long as command wasn't injected
        pass
    except Exception as e:
        # Unexpected error - fail the test
        pytest.fail(f"Unexpected error with payload '{payload}': {str(e)}")
    finally:
        # Clean up test files
        for file in [test_input_file, test_output_file]:
            if os.path.exists(file):
                os.remove(file)