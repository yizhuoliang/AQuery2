import pandas as pd
import time

# Ensure necessary dependencies are installed
try:
    import pandas
except ImportError:
    import os
    os.system('pip install pandas')

# Load the CSV data
df = pd.read_csv('/users/yliang/AQuery2/prices.csv')

# Record the start time
start_time = time.time()

# Ensure the dataframe is sorted by 'month' for the rolling window computation
df = df.sort_values(by='month')

# Compute the 3-month moving average
df['avg_price'] = df['price'].rolling(window=3, min_periods=1).mean()

# Record the end time
end_time = time.time()

# Calculate and print the execution time
execution_time = end_time - start_time
print(f"Execution Time: {execution_time:.4f} seconds")

# If you want to save the dataframe with the new column, you can do:
# df.to_csv('/path/to/save.csv', index=False)
